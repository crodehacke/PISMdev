// Copyright (C) 2012, 2013, 2014, 2015 PISM Authors
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 3 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include <cassert>
#include <cstdio>
#include <deque>
#include <petscvec.h>

#include "PIO.hh"
#include "base/util/IceGrid.hh"
#include "base/util/pism_const.hh"
#include "LocalInterpCtx.hh"
#include "base/util/VariableMetadata.hh"
#include "base/util/PISMConfigInterface.hh"
#include "base/util/PISMTime.hh"
#include "PISMNC3File.hh"
#include "PISMNC4_Quilt.hh"

#if (PISM_USE_PARALLEL_NETCDF4==1)
#include "PISMNC4_Par.hh"
#endif

#if (PISM_USE_PNETCDF==1)
#include "PISMPNCFile.hh"
#endif

#if (PISM_USE_HDF5==1)
#include "PISMNC4_HDF5.hh"
#endif

#include "base/util/pism_memory.hh"
using PISM_SHARED_PTR_NSPACE::shared_ptr;

#include "base/util/error_handling.hh"

namespace pism {

using std::string;
using std::vector;

//! This class collects information needed to perform an array writing operation. It is used by
//! PIO::put_1d_var() to delay writing until PIO::enddef() or PIO::close() are called. This allows
//! us to avoid repeated nc_enddef() and nc_redef() calls when we write metadata and coordinate
//! variables.
class WriteOperation {
public:
  WriteOperation(const std::string &name,
                 const std::vector<unsigned int> &s,
                 const std::vector<unsigned int> &c,
                 const std::vector<double> &array)
    : m_variable_name(name), m_start(s), m_count(c), m_data(array) {
    // empty
  }
  virtual void execute(const PIO &nc) {
    nc.put_vara_double(m_variable_name, m_start, m_count, &m_data[0]);
  }
protected:
  std::string m_variable_name;
  std::vector<unsigned int> m_start;
  std::vector<unsigned int> m_count;
  std::vector<double> m_data;
};

struct PIO::Impl {
  MPI_Comm com;
  std::string backend_type;
  io::NCFile::Ptr nc;
  std::deque<WriteOperation> delayed_writes;
};

static void execute_ops(const PIO &nc, std::deque<WriteOperation> &ops) {
  while (not ops.empty()) {
    ops.front().execute(nc);
    ops.pop_front();
  }
}

static io::NCFile::Ptr create_backend(MPI_Comm com, string mode) {
  if (mode == "netcdf3") {
    return io::NCFile::Ptr(new io::NC3File(com));
  } else if (mode.find("quilt") == 0) {
    size_t n = mode.find(":");
    int compression_level = 0;

    if (n != string::npos) {
      mode.replace(0, 6, "");   // 6 is the length of "quilt:"
      char *endptr;
      compression_level = strtol(mode.c_str(), &endptr, 10);
      if ((*endptr != '\0') || (compression_level < 0) || (compression_level > 9)) {
        PetscErrorCode ierr;
        ierr = PetscPrintf(com, "PISM WARNING: invalid compression level %s."
                           " Output compression is disabled.\n",
                           mode.c_str());
        PISM_CHK(ierr, "PetscPrintf");
        compression_level = 0;
      }
    }

    return io::NCFile::Ptr(new io::NC4_Quilt(com, compression_level));
  }
#if (PISM_USE_PARALLEL_NETCDF4==1)
  else if (mode == "netcdf4_parallel") {
    return io::NCFile::Ptr(new io::NC4_Par(com));
  }
#endif
#if (PISM_USE_PNETCDF==1)
  else if (mode == "pnetcdf") {
    return io::NCFile::Ptr(new io::PNCFile(com));
  }
#endif
#if (PISM_USE_HDF5==1)
  else if (mode == "hdf5") {
    return io::NCFile::Ptr(new io::NC4_HDF5(com));
  }
#endif
  else {
    return io::NCFile::Ptr();       // a "NULL" pointer
  }
}

PIO::PIO(MPI_Comm c, const string &mode)
  : m_impl(new Impl) {
  m_impl->com  = c;
  m_impl->backend_type = mode;
  m_impl->nc   = create_backend(m_impl->com, m_impl->backend_type);

  if (mode != "guess_mode" && not m_impl->nc) {
    throw RuntimeError("failed to allocate an I/O backend (class PIO)");
  }
}

PIO::~PIO() {
  if (m_impl->nc and not inq_filename().empty()) {
    try {
      // a file is still open, so we try to close it
      this->close();
    } catch (...) {
      // don't ever throw from here
      handle_fatal_errors(MPI_COMM_SELF);
    }
  }
  delete m_impl;
}

MPI_Comm PIO::com() const {
  return m_impl->com;
}

// Chooses the best I/O backend for reading from 'filename'.
void PIO::detect_mode(const string &filename) {
  assert(not (bool)m_impl->nc);

  string format;
  {
    io::NC3File nc3(m_impl->com);

    // detect_mode is private, so the caller will handle the failure
    // of open and add context
    nc3.open(filename, PISM_READONLY);
    format = nc3.get_format();
    nc3.close();
  }

  vector<string> modes;
  if (format == "netcdf4") {
    modes.push_back("netcdf4_parallel");
    modes.push_back("netcdf3");
  } else {
    modes.push_back("pnetcdf");
    modes.push_back("netcdf3");
  }

  for (unsigned int j = 0; j < modes.size(); ++j) {
    m_impl->nc = create_backend(m_impl->com, modes[j]);

    if (m_impl->nc) {
      m_impl->backend_type = modes[j];
#if (PISM_DEBUG==1)
      verbPrintf(3, m_impl->com,
                 "  - Using the %s backend to read from %s...\n",
                 modes[j].c_str(), filename.c_str());
#endif
      break;
    }
  }

  if (not m_impl->nc) {
    throw RuntimeError("failed to allocate an I/O backend (class PIO)");
  }
}

std::string PIO::backend_type() const {
  return m_impl->backend_type;
}

void PIO::open(const string &filename, IO_Mode mode) {
  try {

    if (mode == PISM_READONLY || mode == PISM_READWRITE) {
      if (not m_impl->nc and m_impl->backend_type == "guess_mode") {
        detect_mode(filename);
      }
    }

    // opening for reading
    if (mode == PISM_READONLY) {

      assert((bool)m_impl->nc);
      m_impl->nc->open(filename, mode);

    } else if (mode == PISM_READWRITE_CLOBBER ||
               mode == PISM_READWRITE_MOVE) {

      assert((bool)m_impl->nc);

      if (mode == PISM_READWRITE_MOVE) {
        m_impl->nc->move_if_exists(filename);
      } else {
        m_impl->nc->remove_if_exists(filename);
      }

      m_impl->nc->create(filename);

      int old_fill;
      m_impl->nc->set_fill(PISM_NOFILL, old_fill);
    } else if (mode == PISM_READWRITE) {                      // mode == PISM_READWRITE
      assert((bool)m_impl->nc);

      m_impl->nc->open(filename, mode);

      int old_fill;
      m_impl->nc->set_fill(PISM_NOFILL, old_fill);
    } else {
      throw RuntimeError::formatted("invalid mode: %d", mode);
    }
  } catch (RuntimeError &e) {
    e.add_context("opening or creating \"" + filename + "\"");
    throw;
  }
}


void PIO::close() {
  try {
    execute_ops(*this, m_impl->delayed_writes);
    m_impl->nc->close();
  } catch (RuntimeError &e) {
    e.add_context("closing \"" + inq_filename() + "\"");
    throw;
  }
}

void PIO::redef() const {
  try {
    m_impl->nc->redef();
  } catch (RuntimeError &e) {
    e.add_context("switching to define mode; file \"" + inq_filename() + "\"");
    throw;
  }
}


void PIO::enddef() const {
  try {
    m_impl->nc->enddef();
    execute_ops(*this, m_impl->delayed_writes);
  } catch (RuntimeError &e) {
    e.add_context("switching to data mode; file \"" + inq_filename() + "\"");
    throw;
  }
}

string PIO::inq_filename() const {
  return m_impl->nc->get_filename();
}


//! \brief Get the number of records. Uses the length of an unlimited dimension.
unsigned int PIO::inq_nrecords() const {
  try {
    string dim;
    m_impl->nc->inq_unlimdim(dim);

    if (dim.empty()) {
      return 1;                 // one record
    } else {
      return this->inq_dimlen(dim);
    }
  } catch (RuntimeError &e) {
    e.add_context("getting the number of records in file \"" + inq_filename() + "\"");
    throw;
  }
  return 0;                     // will never happen
}

//! \brief Get the number of records of a certain variable. Uses the length of
//! an associated "time" dimension.
unsigned int PIO::inq_nrecords(const string &name, const string &std_name,
                               units::System::Ptr unit_system) const {
  try {
    bool exists = false, found_by_standard_name = false;
    string name_found;
    inq_var(name, std_name, exists, name_found, found_by_standard_name);

    if (exists == false) {
      return 0;
    }

    vector<string> dims;
    m_impl->nc->inq_vardimid(name_found, dims);

    for (unsigned int j = 0; j < dims.size(); ++j) {
      AxisType dimtype = inq_dimtype(dims[j], unit_system);

      if (dimtype == T_AXIS) {
        return this->inq_dimlen(dims[j]);
      }
    }

    return 1;                   // one record
  } catch (RuntimeError &e) {
    e.add_context("getting the number of records of variable '%s' ('%s') in '%s'",
                  name.c_str(), std_name.c_str(), inq_filename().c_str());
    throw;
  }
  return 0;                     // will never happen
}


//! \brief Find a variable using its standard name and/or short name.
/*!
 * Sets "result" to the short name found.
 */
void PIO::inq_var(const string &short_name, const string &std_name, bool &exists,
                  string &result, bool &found_by_standard_name) const {
  try {
    exists = false;

    if (std_name.empty() == false) {

      int nvars;
      m_impl->nc->inq_nvars(nvars);

      for (int j = 0; j < nvars; ++j) {
        string name;
        m_impl->nc->inq_varname(j, name);

        string attribute = get_att_text(name, "standard_name");

        if (attribute.empty()) {
          continue;
        }

        if (attribute == std_name) {
          if (exists == false) {
            exists = true;
            found_by_standard_name = true;
            result = name;
          } else {
            throw RuntimeError::formatted("inconsistency in '%s': variables '%s' and '%s'\n"
                                          "have the same standard_name (%s)",
                                          inq_filename().c_str(), result.c_str(),
                                          name.c_str(), attribute.c_str());
          }
        }

      } // end of the for loop
    } // end of if (std_name.empty() == false)

    if (exists == false) {
      m_impl->nc->inq_varid(short_name, exists);
      if (exists == true) {
        result = short_name;
      } else {
        result.clear();
      }

      found_by_standard_name = false;
    }

  } catch (RuntimeError &e) {
    e.add_context("searching for variable '%s' ('%s') in '%s'", short_name.c_str(), std_name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Checks if a variable exists.
bool PIO::inq_var(const string &name) const {
  try {
    bool exists = false;
    m_impl->nc->inq_varid(name, exists);
    return exists;
  } catch (RuntimeError &e) {
    e.add_context("searching for variable '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}

vector<string> PIO::inq_vardims(const string &name) const {
  try {
    vector<string> result;
    m_impl->nc->inq_vardimid(name, result);
    return result;
  } catch (RuntimeError &e) {
    e.add_context("getting dimensions of variable '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}


//! \brief Checks if a dimension exists.
bool PIO::inq_dim(const string &name) const {
  try {
    bool exists = false;
    m_impl->nc->inq_dimid(name, exists);
    return exists;
  } catch (RuntimeError &e) {
    e.add_context("searching for dimension '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Get the length of a dimension.
/*!
 * Sets result to 0 if a dimension does not exist.
 */
unsigned int PIO::inq_dimlen(const string &name) const {
  try {
    bool exists = false;
    m_impl->nc->inq_dimid(name, exists);
    if (exists == true) {
      unsigned int result = 0;
      m_impl->nc->inq_dimlen(name, result);
      return result;
    } else {
      return 0;
    }
  } catch (RuntimeError &e) {
    e.add_context("getting the length of dimension '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Get the "type" of a dimension.
/*!
 * The "type" is one of X_AXIS, Y_AXIS, Z_AXIS, T_AXIS.
 */
AxisType PIO::inq_dimtype(const string &name,
                          units::System::Ptr unit_system) const {
  try {
    string axis, standard_name, units;
    units::Unit tmp_units(unit_system, "1");
    bool exists;

    m_impl->nc->inq_varid(name, exists);
    
    if (exists == false) {
      throw RuntimeError("coordinate variable " + name + " is missing");
    }

    axis          = get_att_text(name, "axis");
    standard_name = get_att_text(name, "standard_name");
    units         = get_att_text(name, "units");

    // check if it has units compatible with "seconds":

    tmp_units = units::Unit(unit_system, units);

    units::Unit seconds(unit_system, "seconds");
    if (units::are_convertible(tmp_units, seconds)) {
      return T_AXIS;
    }

    // check the standard_name attribute:
    if (standard_name == "time") {
      return T_AXIS;
    } else if (standard_name == "projection_x_coordinate") {
      return X_AXIS;
    } else if (standard_name == "projection_y_coordinate") {
      return Y_AXIS;
    }

    // check the axis attribute:
    if (axis == "T" or axis == "t") {
      return T_AXIS;
    } else if (axis == "X" or axis == "x") {
      return X_AXIS;
    } else if (axis == "Y" or axis == "y") {
      return Y_AXIS;
    } else if (axis == "Z" or axis == "z") {
      return Z_AXIS;
    }

    // check the variable name:
    if (name == "x" or name == "X" or
        name.find("x") == 0 or name.find("X") == 0) {
      return X_AXIS;
    } else if (name == "y" or name == "Y" or
               name.find("y") == 0 or name.find("Y") == 0) {
      return Y_AXIS;
    } else if (name == "z" or name == "Z" or
               name.find("z") == 0 or name.find("Z") == 0) {
      return Z_AXIS;
    } else if (name == "t" or name == "T" or name == "time" or
               name.find("t") == 0 or name.find("T") == 0) {
      return T_AXIS;
    }

    // we have no clue:
    return UNKNOWN_AXIS;
  } catch (RuntimeError &e) {
    e.add_context("getting the type of dimension '%s' in '%s'",
                  name.c_str(), inq_filename().c_str());
    throw;
  }
  return UNKNOWN_AXIS;          // will never happen
}

void PIO::inq_dim_limits(const string &name, double *min, double *max) const {
  try {
    vector<double> data;
    get_dim(name, data);

    double my_min = data[0],
      my_max = data[0];
    for (unsigned int j = 0; j < data.size(); ++j) {
      my_min = std::min(data[j], my_min);
      my_max = std::max(data[j], my_max);
    }

    if (min != NULL) {
      *min = my_min;
    }

    if (max != NULL) {
      *max = my_max;
    }

  } catch (RuntimeError &e) {
    e.add_context("getting limits of dimension '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}

void PIO::def_dim(const std::string &name, size_t length) const {
  try {
    m_impl->nc->def_dim(name, length);
  } catch (RuntimeError &e) {
    e.add_context("defining dimension '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}


//! \brief Define a variable.
void PIO::def_var(const string &name, IO_Type nctype, const vector<string> &dims) const {
  try {
    m_impl->nc->def_var(name, nctype, dims);
  } catch (RuntimeError &e) {
    e.add_context("defining variable '%s' in '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}

void PIO::get_1d_var(const string &name, unsigned int s, unsigned int c,
                     vector<double> &result) const {
  vector<unsigned int> start(1), count(1);

  result.resize(c);

  start[0] = s;
  count[0] = c;

  // this call will handle errors
  get_vara_double(name, start, count, &result[0]);
}


//! Write a 1D (usually a coordinate) variable.
/**
 * This method delays writing until the next PIO::enddef() or PIO::close() call.
 *
 * We do this to make it possible to define all variables before writing *any* data, which is needed
 * for better I/O performance.
 */
void PIO::put_1d_var(const string &name, unsigned int s, unsigned int c,
                     const vector<double> &data) const {
  vector<unsigned int> start(1, s), count(1, c);
  WriteOperation op(name, start, count, data);

  // Add this write operation to the stack of operations to be performed when enddef() is called.
  m_impl->delayed_writes.push_back(op);
}


//! \brief Get dimension data (a coordinate variable).
void PIO::get_dim(const string &name, vector<double> &data) const {
  try {
    unsigned int dim_length = 0;
    m_impl->nc->inq_dimlen(name, dim_length);

    get_1d_var(name, 0, dim_length, data);
  } catch (RuntimeError &e) {
    e.add_context("reading dimension '%s' from '%s'", name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Append to the history global attribute.
/*!
 * Use put_att_text("PISM_GLOBAL", "history", ...) to overwrite "history".
 */
void PIO::append_history(const string &history) const {
  try {
    string old_history = get_att_text("PISM_GLOBAL", "history");
    put_att_text("PISM_GLOBAL", "history", history + old_history);
  } catch (RuntimeError &e) {
    e.add_context("appending to the history attribute in \"" + inq_filename() + "\"");
    throw;
  }
}

//! \brief Write a multiple-valued double attribute.
void PIO::put_att_double(const string &var_name, const string &att_name, IO_Type nctype,
                         const vector<double> &values) const {
  try {
    m_impl->nc->redef();
    m_impl->nc->put_att_double(var_name, att_name, nctype, values);
  } catch (RuntimeError &e) {
    e.add_context("writing double attribute '%s:%s' in '%s'",
                  var_name.c_str(), att_name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Write a single-valued double attribute.
void PIO::put_att_double(const string &var_name, const string &att_name, IO_Type nctype,
                         double value) const {
  vector<double> tmp(1, value);

  // this call will handle errors
  this->put_att_double(var_name, att_name, nctype, tmp);
}

//! \brief Write a text attribute.
void PIO::put_att_text(const string &var_name, const string &att_name,
                       const string &value) const {
  try {
    m_impl->nc->redef();

    string tmp = value + "\0";    // ensure that the string is null-terminated

    m_impl->nc->put_att_text(var_name, att_name, tmp);
  } catch (RuntimeError &e) {
    e.add_context("writing text attribute '%s:%s' in '%s'",
                  var_name.c_str(), att_name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Get a double attribute.
vector<double> PIO::get_att_double(const string &var_name, const string &att_name) const {
  try {
    IO_Type att_type;
    m_impl->nc->inq_atttype(var_name, att_name, att_type);

    // Give an understandable error message if a string attribute was found when
    // a number (or a list of numbers) was expected. (We've seen datasets with
    // "valid_min" stored as a string...)
    if (att_type == PISM_CHAR) {
      string tmp = get_att_text(var_name, att_name);

      throw RuntimeError::formatted("attribute %s is a string '%s'; expected a number or a list of numbers",
                                    att_name.c_str(), tmp.c_str());
    } else {
      // In this case att_type might be PISM_NAT (if an attribute does not
      // exist), but get_att_double can handle that.
      vector<double> result;
      m_impl->nc->get_att_double(var_name, att_name, result);
      return result;
    }
  } catch (RuntimeError &e) {
    e.add_context("reading double attribute '%s:%s' from '%s'",
                  var_name.c_str(), att_name.c_str(), inq_filename().c_str());
    throw;
  }
}

//! \brief Get a text attribute.
string PIO::get_att_text(const string &var_name, const string &att_name) const {
  try {
    string result;
    m_impl->nc->get_att_text(var_name, att_name, result);
    return result;
  } catch (RuntimeError &e) {
    e.add_context("reading text attribute '%s:%s' from %s", var_name.c_str(), att_name.c_str(), inq_filename().c_str());
    throw;
  }
}

unsigned int PIO::inq_nattrs(const string &var_name) const {
  try {
    int result = 0;
    m_impl->nc->inq_varnatts(var_name, result);
    return result;
  } catch (RuntimeError &e) {
    e.add_context("getting the number of attributes of variable '%s' in '%s'", var_name.c_str(), inq_filename().c_str());
    throw;
  }
}


string PIO::inq_attname(const string &var_name, unsigned int n) const {
  try {
    string result;
    m_impl->nc->inq_attname(var_name, n, result);
    return result;
  } catch (RuntimeError &e) {
    e.add_context("getting the name of an attribute of variable '%s' in '%s'", var_name.c_str(), inq_filename().c_str());
    throw;
  }
}


IO_Type PIO::inq_atttype(const string &var_name, const string &att_name) const {
  try {
    IO_Type result;
    m_impl->nc->inq_atttype(var_name, att_name, result);
    return result;
  } catch (RuntimeError &e) {
    e.add_context("getting the type of an attribute of variable '%s' in '%s'", var_name.c_str(), inq_filename().c_str());
    throw;
  }
}


void PIO::get_vara_double(const string &variable_name,
                          const vector<unsigned int> &start,
                          const vector<unsigned int> &count,
                          double *ip) const {
  try {
    m_impl->nc->enddef();
    m_impl->nc->get_vara_double(variable_name, start, count, ip);
  } catch (RuntimeError &e) {
    e.add_context("reading variable '%s' from '%s'", variable_name.c_str(), inq_filename().c_str());
    throw;
  }
}


void PIO::put_vara_double(const string &variable_name,
                          const vector<unsigned int> &start,
                          const vector<unsigned int> &count,
                          const double *op) const {
  try {
    m_impl->nc->enddef();
    m_impl->nc->put_vara_double(variable_name, start, count, op);
  } catch (RuntimeError &e) {
    e.add_context("writing variable '%s' to '%s'", variable_name.c_str(), inq_filename().c_str());
    throw;
  }
}

void PIO::get_varm_double(const string &variable_name,
                          const vector<unsigned int> &start,
                          const vector<unsigned int> &count,
                          const vector<unsigned int> &imap, double *ip) const {
  try {
    m_impl->nc->enddef();
    m_impl->nc->get_varm_double(variable_name, start, count, imap, ip);
  } catch (RuntimeError &e) {
    e.add_context("reading variable '%s' from '%s'", variable_name.c_str(), inq_filename().c_str());
    throw;
  }
}

void PIO::put_varm_double(const string &variable_name,
                          const vector<unsigned int> &start,
                          const vector<unsigned int> &count,
                          const vector<unsigned int> &imap,
                          const double *op) const {
  try {
    m_impl->nc->enddef();
    m_impl->nc->put_varm_double(variable_name, start, count, imap, op);
  } catch (RuntimeError &e) {
    e.add_context("writing variable '%s' to '%s'", variable_name.c_str(), inq_filename().c_str());
    throw;
  }
}

void PIO::set_local_extent(unsigned int xs, unsigned int xm,
                           unsigned int ys, unsigned int ym) const {
  m_impl->nc->set_local_extent(xs, xm, ys, ym);
}


} // end of namespace pism
