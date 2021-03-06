// Copyright (C) 2008--2015 Ed Bueler and Constantine Khroulev
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

#include <gsl/gsl_math.h>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <petscdmda.h>

#include <cassert>

#include "pism_memory.hh"
using PISM_SHARED_PTR_NSPACE::dynamic_pointer_cast;

#include "base/util/io/PIO.hh"
#include "iceModelVec.hh"
#include "IceGrid.hh"
#include "PISMConfigInterface.hh"

#include "error_handling.hh"

namespace pism {

// this file contains method for derived class IceModelVec3

// methods for base class IceModelVec and derived class IceModelVec2S
// are in "iceModelVec.cc"

IceModelVec3D::IceModelVec3D()
  : IceModelVec() {
  m_bsearch_accel = gsl_interp_accel_alloc();
  if (m_bsearch_accel == NULL) {
    throw RuntimeError("Failed to allocate a GSL interpolation accelerator");
  }
}

IceModelVec3D::~IceModelVec3D() {
  gsl_interp_accel_free(m_bsearch_accel);
}

IceModelVec3::IceModelVec3() {
  // empty
}

IceModelVec3::~IceModelVec3() {
  // empty
}


IceModelVec3::Ptr IceModelVec3::To3DScalar(IceModelVec::Ptr input) {
  IceModelVec3::Ptr result = dynamic_pointer_cast<IceModelVec3,IceModelVec>(input);
  if (not (bool)result) {
    throw RuntimeError("dynamic cast failure");
  }
  return result;
}

//! Allocate a DA and a Vec from information in IceGrid.
void IceModelVec3D::allocate(IceGrid::ConstPtr my_grid, const std::string &my_name,
                             IceModelVecKind ghostedp, const std::vector<double> &levels,
                             unsigned int stencil_width) {
  PetscErrorCode ierr;
  m_grid = my_grid;

  zlevels = levels;
  m_da_stencil_width = stencil_width;

  m_da = m_grid->get_dm(this->zlevels.size(), this->m_da_stencil_width);

  m_has_ghosts = (ghostedp == WITH_GHOSTS);

  if (m_has_ghosts == true) {
    ierr = DMCreateLocalVector(*m_da, m_v.rawptr());
    PISM_CHK(ierr, "DMCreateLocalVector");
  } else {
    ierr = DMCreateGlobalVector(*m_da, m_v.rawptr());
    PISM_CHK(ierr, "DMCreateGlobalVector");
  }

  m_name = my_name;

  m_metadata.push_back(SpatialVariableMetadata(m_grid->ctx()->unit_system(),
                                               my_name, zlevels));
}

bool IceModelVec3D::isLegalLevel(double z) const {
  double z_min = zlevels.front(),
    z_max = zlevels.back();
  if (z < z_min - 1.0e-6 || z > z_max + 1.0e-6) {
    return false;
  }
  return true;
}

//! Set all values of scalar quantity to given a single value in a particular column.
void IceModelVec3D::set_column(int i, int j, double c) {
  PetscErrorCode ierr;
#if (PISM_DEBUG==1)
  assert(array != NULL);
  check_array_indices(i, j, 0);
#endif

  double ***arr = (double***) array;

  if (c == 0.0) {
    ierr = PetscMemzero(arr[i][j], zlevels.size() * sizeof(double));
    PISM_CHK(ierr, "PetscMemzero");
  } else {
    unsigned int nlevels = zlevels.size();
    for (unsigned int k=0; k < nlevels; k++) {
      arr[i][j][k] = c;
    }
  }
}

//! Return value of scalar quantity at level z (m) above base of ice (by linear interpolation).
double IceModelVec3D::getValZ(int i, int j, double z) const {
#if (PISM_DEBUG==1)
  assert(array != NULL);
  check_array_indices(i, j, 0);

  if (not isLegalLevel(z)) {
    throw RuntimeError::formatted("IceModelVec3 getValZ(): level %f is not legal; name = %s",
                                  z, m_name.c_str());
  }
#endif

  double ***arr = (double***) array;
  if (z >= zlevels.back()) {
    unsigned int nlevels = zlevels.size();
    return arr[i][j][nlevels - 1];
  } else if (z <= zlevels.front()) {
    return arr[i][j][0];
  }

  unsigned int mcurr = gsl_interp_accel_find(m_bsearch_accel, &zlevels[0], zlevels.size(), z);

  const double incr = (z - zlevels[mcurr]) / (zlevels[mcurr+1] - zlevels[mcurr]);
  const double valm = arr[i][j][mcurr];
  return valm + incr * (arr[i][j][mcurr+1] - valm);
}

//! Copies a horizontal slice at level z of an IceModelVec3 into a Vec gslice.
/*!
 * FIXME: this method is misnamed: the slice is horizontal in the PISM
 * coordinate system, not in reality.
 */
void  IceModelVec3::getHorSlice(Vec &gslice, double z) const {

  petsc::DM::Ptr da2 = m_grid->get_dm(1, m_grid->ctx()->config()->get_double("grid_max_stencil_width"));

  IceModelVec::AccessList list(*this);
  petsc::DMDAVecArray slice(da2, gslice);
  double **slice_val = (double**)slice.get();

  ParallelSection loop(m_grid->com);
  try {
    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();
      slice_val[i][j] = getValZ(i,j,z);
    }
  } catch (...) {
    loop.failed();
  }
  loop.check();
}

//! Copies a horizontal slice at level z of an IceModelVec3 into an IceModelVec2S gslice.
/*!
 * FIXME: this method is misnamed: the slice is horizontal in the PISM
 * coordinate system, not in reality.
 */
void  IceModelVec3::getHorSlice(IceModelVec2S &gslice, double z) const {
  IceModelVec::AccessList list(*this);
  list.add(gslice);

  ParallelSection loop(m_grid->com);
  try {
    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();
      gslice(i, j) = getValZ(i, j, z);
    }
  } catch (...) {
    loop.failed();
  }
  loop.check();
}


//! Copies the values of an IceModelVec3 at the ice surface (specified by the level myH) to an IceModelVec2S gsurf.
void IceModelVec3::getSurfaceValues(IceModelVec2S &surface_values,
                                    const IceModelVec2S &H) const {
  IceModelVec::AccessList list(*this);
  list.add(surface_values);
  list.add(H);

  ParallelSection loop(m_grid->com);
  try {
    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();
      surface_values(i, j) = getValZ(i, j, H(i, j));
    }
  } catch (...) {
    loop.failed();
  }
  loop.check();
}

double* IceModelVec3D::get_column(int i, int j) {
#if (PISM_DEBUG==1)
  check_array_indices(i, j, 0);
#endif
  return ((double***) array)[i][j];
}

const double* IceModelVec3D::get_column(int i, int j) const {
#if (PISM_DEBUG==1)
  check_array_indices(i, j, 0);
#endif
  return ((double***) array)[i][j];
}

void  IceModelVec3D::set_column(int i, int j, double *valsIN) {
#if (PISM_DEBUG==1)
  check_array_indices(i, j, 0);
#endif
  double ***arr = (double***) array;
  PetscErrorCode ierr = PetscMemcpy(arr[i][j], valsIN, zlevels.size()*sizeof(double));
  PISM_CHK(ierr, "PetscMemcpy");
}

void  IceModelVec3::create(IceGrid::ConstPtr my_grid, const std::string &my_name,
                           IceModelVecKind ghostedp,
                           unsigned int stencil_width) {

  IceModelVec3D::allocate(my_grid, my_name, ghostedp,
                          my_grid->z(), stencil_width);
}

} // end of namespace pism
