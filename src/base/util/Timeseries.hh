// Copyright (C) 2009, 2011, 2012, 2013, 2014, 2015 Constantine Khroulev
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

#ifndef __Timeseries_hh
#define __Timeseries_hh

#include <deque>
#include <mpi.h>

#include "VariableMetadata.hh"

namespace pism {

class IceGrid;
class PIO;
class Time;
class Logger;

//! \brief A general class for reading and accessing time-series.
/*!
  \section timeseries_overview Scalar time-series

  This class provides random access to time-series values. It is used to
  implement forcing with scalar time-dependent parameters (such as
  paleo-climate forcing).

  Note that every processor stores the whole time-series and calling append()
  repeatedly will use a lot of memory.

  Please use DiagnosticTimeseries to output long time-series.

  \subsection timeseries_example An example

  The following snippet from PAForcing::init() illustrates creating a Timeseries
  object and reading data from a file.

  \code
  delta_T = new Timeseries(grid.com, grid.rank, "delta_T", "time");
  ierr = delta_T->set_string("units", "Kelvin", ""); CHKERRQ(ierr);
  ierr = delta_T->set_dimension_units("years", ""); CHKERRQ(ierr);
  ierr = delta_T->set_attr("long_name", "near-surface air temperature offsets");
  CHKERRQ(ierr);
  
  ierr = delta_T->read(dT_file); CHKERRQ(ierr);
  \endcode

  Call
  \code
  double offset = (*delta_T)(time);
  \endcode
  to get the value corresponding to the time "time", in this case in years. The
  value returned will be computed using linear interpolation.

  It is also possible to get an n-th value from a time-series: just use square brackets:
  \code
  double offset = (*delta_T)[10];
  \endcode
*/
class Timeseries {
public:
  Timeseries(const IceGrid &g, const std::string &name, const std::string &dimension_name);
  Timeseries(MPI_Comm com, units::System::Ptr units_system,
             const std::string &name, const std::string &dimension_name);
  
  void read(const PIO &nc, const Time &time_manager, const Logger &log);
  void write(const PIO &nc);
  double operator()(double time);
  double operator[](unsigned int j) const;
  double average(double t, double dt, unsigned int N);
  void append(double value, double a, double b);
  int length();

  TimeseriesMetadata& metadata();
  TimeseriesMetadata& dimension_metadata();

  void scale(double scaling_factor);

  std::string short_name;
protected:
  void set_bounds_units();
  TimeseriesMetadata m_dimension, m_variable;
  MPI_Comm m_com;
  TimeBoundsMetadata m_bounds;
  bool m_use_bounds;
  std::vector<double> m_time;
  std::vector<double> m_values;
  std::vector<double> m_time_bounds;
private:
  void private_constructor(MPI_Comm com, const std::string &name, const std::string &dimension_name);
  void report_range(const Logger &log);
};

//! A class for storing and writing diagnostic time-series.
/*! This version of Timeseries only holds `buffer_size` entries in memory and
  writes to a file every time this limit is exceeded.

  Here is a usage example:

  First, prepare a file for writing:

  \code
  std::string seriesname = "ser_delta_T.nc";
  PIO nc(grid.com, grid.rank, grid.config.get_string("output_format"));
  nc.open_for_writing(seriesname, true, false);
  nc.close();
  \endcode

  Next, create the DiagnosticTimeseries object and set metadata. This will
  prepare the offsets object to write delta_T(t) time-series to file
  ser_delta_T.nc, converting from degrees Kelvin (internal units) to degrees
  Celsius ("glaciological" units). Time will be written in seconds (%i.e. there is
  no unit conversion there).

  \code
  offsets = new DiagnosticTimeseries(g, "delta_T", "time");
  offsets->set_string("units", "Kelvin", "Celsius");
  offsets->set_dimension_units("seconds", "");
  offsets->buffer_size = 100; // only store 100 entries; default is 10000
  offsets->output_filename = seriesname;
  offsets->set_attr("long_name", "temperature offsets from some value");
  \endcode

  Once this is set up, one can add calls like

  \code
  offsets->append(my_t - my_dt, my_t, TsOffset);
  offsets->interp(time - my_dt, time);
  \endcode

  to the code. The first call will store the (my_t, TsOffset). The second
  call will use linear interpolation to find the value at `time` years.  Note
  that the first call adds to a buffer but does not yield any output without 
  the second call.  Therefore, even if interpolation is not really needed
  because time==my_t, the call to interp() should still occur.
  
  Finally, the destructor of DiagnosticTimeseries will flush(), which writes out
  the buffered values:

  \code
  delete offsets;
  \endcode

  Note that every time you exceed the `buffer_size` limit, all the entries are
  written to a file by flush() <b> and removed from memory</b>.  One may also
  explicitly call flush().
*/
class DiagnosticTimeseries : public Timeseries {
public:
  DiagnosticTimeseries(const IceGrid &g, const std::string &name, const std::string &dimension_name);
  ~DiagnosticTimeseries();

  void init(const std::string &filename);
  void append(double V, double a, double b);
  void interp(double a, double b);
  void reset();
  void flush();

  size_t buffer_size;
  std::string output_filename;
  bool rate_of_change;

protected:
  size_t m_start;
  std::deque<double> m_t, m_v;
  double m_v_previous;
};

} // end of namespace pism

#endif // __Timeseries_hh
