// Copyright (C) 2004-2015 Jed Brown, Ed Bueler and Constantine Khroulev
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

#include <map>
#include <petscsys.h>
#include <gsl/gsl_interp.h>

#include "IceGrid.hh"
#include "pism_const.hh"
#include "PISMTime.hh"
#include "PISMTime_Calendar.hh"
#include "PISMConfigInterface.hh"
#include "pism_options.hh"
#include "error_handling.hh"
#include "base/util/io/PIO.hh"
#include "base/util/PISMVars.hh"
#include "base/util/Logger.hh"

namespace pism {

//! Internal structures of IceGrid.
struct IceGrid::Impl {
  Context::Ptr ctx;

  // int to match types used by MPI
  int rank;
  int size;

  //! @brief array containing lenghts (in the x-direction) of processor sub-domains
  std::vector<PetscInt> procs_x;
  //! @brief array containing lenghts (in the y-direction) of processor sub-domains
  std::vector<PetscInt> procs_y;

  Periodicity periodicity;

  //! x-coordinates of grid points
  std::vector<double> x;
  //! y-coordinates of grid points
  std::vector<double> y;
  //! vertical grid levels in the ice; correspond to the storage grid
  std::vector<double> z;

  int xs, xm, ys, ym;
  //! horizontal grid spacing
  double dx;
  //! horizontal grid spacing
  double dy;
  //! number of grid points in the x-direction
  unsigned int Mx;
  //! number of grid points in the y-direction
  unsigned int My;

  //! x-coordinate of the grid center
  double x0;
  //! y-coordinate of the grid center
  double y0;

  //! half width of the ice model grid in x-direction (m)
  double Lx;
  //! half width of the ice model grid in y-direction (m)
  double Ly;

  std::map<int,petsc::DM::WeakPtr> dms;

  // This DM is used for I/O operations and is not owned by any
  // IceModelVec (so far, anyway). We keep a pointer to it here to
  // avoid re-allocating it many times.
  petsc::DM::Ptr dm_scalar_global;

  //! @brief A dictionary with pointers to IceModelVecs, for passing
  //! them from the one component to another (e.g. from IceModel to
  //! surface and ocean models).
  Vars variables;

  //! GSL binary search accelerator used to speed up kBelowHeight().
  gsl_interp_accel *bsearch_accel;
};

//! Convert a string to Periodicity.
Periodicity string_to_periodicity(const std::string &keyword) {
    if (keyword == "none") {
    return NOT_PERIODIC;
  } else if (keyword == "x") {
    return X_PERIODIC;
  } else if (keyword == "y") {
    return Y_PERIODIC;
  } else if (keyword == "xy") {
    return XY_PERIODIC;
  } else {
    throw RuntimeError::formatted("grid periodicity type '%s' is invalid.",
                                  keyword.c_str());
  }
}

//! Convert Periodicity to a STL string.
std::string periodicity_to_string(Periodicity p) {
  switch (p) {
  case NOT_PERIODIC:
    return "none";
  case X_PERIODIC:
    return "x";
  case Y_PERIODIC:
    return "y";
  default:
  case XY_PERIODIC:
    return "xy";
  }
}

//! Convert an STL string to SpacingType.
SpacingType string_to_spacing(const std::string &keyword) {
  if (keyword == "quadratic") {
    return QUADRATIC;
  } else if (keyword == "equal") {
    return EQUAL;
  } else {
    throw RuntimeError::formatted("ice vertical spacing type '%s' is invalid.",
                                  keyword.c_str());
  }
}

//! Convert SpacingType to an STL string.
std::string spacing_to_string(SpacingType s) {
  switch (s) {
  case EQUAL:
    return "equal";
  default:
  case QUADRATIC:
    return "quadratic";
  }
}

/*! @brief Initialize a uniform, shallow (3 z-levels) grid with half-widths (Lx,Ly) and Mx by My
 * nodes.
 */
IceGrid::Ptr IceGrid::Shallow(Context::Ptr ctx,
                              double Lx, double Ly,
                              double x0, double y0,
                              unsigned int Mx, unsigned int My,
                              Periodicity periodicity) {
  try {
    GridParameters p(ctx->config());
    p.Lx = Lx;
    p.Ly = Ly;
    p.x0 = x0;
    p.y0 = y0;
    p.Mx = Mx;
    p.My = My;
    p.periodicity = periodicity;

    double Lz = ctx->config()->get_double("grid_Lz");
    p.z.resize(3);
    p.z[0] = 0.0;
    p.z[1] = 0.5 * Lz;
    p.z[2] = 1.0 * Lz;

    p.ownership_ranges_from_options(ctx->size());

    return IceGrid::Ptr(new IceGrid(ctx, p));
  } catch (RuntimeError &e) {
    e.add_context("initializing a shallow grid");
    throw;
  }
}

//! @brief Create a PISM distributed computational grid.
/*!
  This class contains the "fundamental" transpose: "My,Mx" instead of "Mx,My"
  in the DMDACreate2d call; this transpose allows us to index arrays by "[i][j]"
  (where 'i' corresponds to 'x' and 'j' to 'y') and be consistent about
  meanings of 'x', 'y', 'u' and 'v'.

  Unfortunately this means that PETSc viewers appear transposed.

  This choice should be virtually invisible, unless you're using DALocalInfo
  and MatStencil structures.

  @note PETSc order: x in columns, y in rows, indexing as array[y][x]. PISM
  order: x in rows, y in columns, indexing as array[x][y].
 */
IceGrid::IceGrid(Context::Ptr context, const GridParameters &p)
  : com(context->com()), m_impl(new Impl) {

  try {  m_impl->ctx = context;

    m_impl->bsearch_accel = gsl_interp_accel_alloc();
    if (m_impl->bsearch_accel == NULL) {
      throw RuntimeError("Failed to allocate a GSL interpolation accelerator");
    }

    MPI_Comm_rank(com, &m_impl->rank);
    MPI_Comm_size(com, &m_impl->size);

    p.validate();

    m_impl->Mx = p.Mx;
    m_impl->My = p.My;
    m_impl->x0 = p.x0;
    m_impl->y0 = p.y0;
    m_impl->Lx = p.Lx;
    m_impl->Ly = p.Ly;
    m_impl->periodicity = p.periodicity;
    m_impl->z = p.z;
    this->set_ownership_ranges(p.procs_x, p.procs_y);

    compute_horizontal_coordinates();

    {
      unsigned int max_stencil_width = (unsigned int)context->config()->get_double("grid_max_stencil_width");

      try {
        petsc::DM::Ptr tmp = this->get_dm(1, max_stencil_width);
      } catch (RuntimeError &e) {
        e.add_context("distributing a %d x %d grid across %d processors.",
                      Mx(), My(), size());
        throw;
      }

      // hold on to a DM corresponding to dof=1, stencil_width=0 (it will
      // be needed for I/O operations)
      m_impl->dm_scalar_global = this->get_dm(1, 0);

      DMDALocalInfo info;
      PetscErrorCode ierr = DMDAGetLocalInfo(*m_impl->dm_scalar_global, &info);
      PISM_CHK(ierr, "DMDAGetLocalInfo");

      // this continues the fundamental transpose
      m_impl->xs = info.ys;
      m_impl->xm = info.ym;
      m_impl->ys = info.xs;
      m_impl->ym = info.xm;

    }
  } catch (RuntimeError &e) {
    e.add_context("allocating IceGrid");
    throw;
  }
}

//! Create a grid using one of variables in `var_names` in `file`.
IceGrid::Ptr IceGrid::FromFile(Context::Ptr ctx,
                               const std::string &filename,
                               const std::vector<std::string> &var_names,
                               Periodicity periodicity) {

  for (unsigned int k = 0; k < var_names.size(); ++k) {
    PIO file(ctx->com(), "netcdf3");
    file.open(filename, PISM_READONLY); // will be closed automatically
    if (file.inq_var(var_names[k])) {
      return FromFile(ctx, filename, var_names[k], periodicity);
    }
  }

  throw RuntimeError::formatted("file %s does not have any of %s."
                                " Cannot initialize the grid.",
                                filename.c_str(),
                                join(var_names, ",").c_str());
}

//! Create a grid from a file, get information from variable `var_name`.
IceGrid::Ptr IceGrid::FromFile(Context::Ptr ctx,
                               const std::string &filename,
                               const std::string &var_name,
                               Periodicity periodicity) {
  try {
    const Logger &log = *ctx->log();

    // The following call may fail because var_name does not exist. (And this is fatal!)
    // Note that this sets defaults using configuration parameters, too.
    GridParameters p(ctx, filename, var_name, periodicity);

    // if we have no vertical grid information, create a fake 2-level vertical grid.
    if (p.z.size() < 2) {
      double Lz = ctx->config()->get_double("grid_Lz");
      log.message(3,
                 "WARNING: Can't determine vertical grid information using '%s' in %s'\n"
                 "         Using 2 levels and Lz of %3.3fm\n",
                 var_name.c_str(), filename.c_str(), Lz);

      p.z.clear();
      p.z.push_back(0);
      p.z.push_back(Lz);
    }

    // override periodicity
    p.periodicity = periodicity;
    p.ownership_ranges_from_options(ctx->size());

    return IceGrid::Ptr(new IceGrid(ctx, p));
  } catch (RuntimeError &e) {
    e.add_context("initializing computational grid from variable \"%s\" in \"%s\"",
                  var_name.c_str(), filename.c_str());
    throw;
  }
}

IceGrid::~IceGrid() {
  gsl_interp_accel_free(m_impl->bsearch_accel);
  delete m_impl;
}

//! \brief Set the vertical levels in the ice according to values in `Mz` (number of levels), `Lz`
//! (domain height), `spacing` (quadratic or equal) and `lambda` (quadratic spacing parameter).
/*!
  - When `vertical_spacing == EQUAL`, the vertical grid in the ice is equally spaced:
    `zlevels[k] = k dz` where `dz = Lz / (Mz - 1)`.
  - When `vertical_spacing == QUADRATIC`, the spacing is a quadratic function.  The intent
    is that the spacing is smaller near the base than near the top.

    In particular, if
    \f$\zeta_k = k / (\mathtt{Mz} - 1)\f$ then `zlevels[k] = Lz *`
    ((\f$\zeta_k\f$ / \f$\lambda\f$) * (1.0 + (\f$\lambda\f$ - 1.0)
    * \f$\zeta_k\f$)) where \f$\lambda\f$ = 4.  The value \f$\lambda\f$
    indicates the slope of the quadratic function as it leaves the base.
    Thus a value of \f$\lambda\f$ = 4 makes the spacing about four times finer
    at the base than equal spacing would be.
 */
std::vector<double> IceGrid::compute_vertical_levels(double new_Lz, unsigned int new_Mz,
                                                     SpacingType spacing, double lambda) {

  if (new_Mz < 2) {
    throw RuntimeError("Mz must be at least 2");
  }

  if (new_Lz <= 0) {
    throw RuntimeError("Lz must be positive");
  }

  if (spacing == QUADRATIC and lambda <= 0) {
    throw RuntimeError("lambda must be positive");
  }

  std::vector<double> result(new_Mz);

  // Fill the levels in the ice:
  switch (spacing) {
  case EQUAL: {
    double dz = new_Lz / ((double) new_Mz - 1);

    // Equal spacing
    for (unsigned int k=0; k < new_Mz - 1; k++) {
      result[k] = dz * ((double) k);
    }
    result[new_Mz - 1] = new_Lz;  // make sure it is exactly equal
    break;
  }
  case QUADRATIC: {
    // this quadratic scheme is an attempt to be less extreme in the fineness near the base.
    for (unsigned int k=0; k < new_Mz - 1; k++) {
      const double zeta = ((double) k) / ((double) new_Mz - 1);
      result[k] = new_Lz * ((zeta / lambda) * (1.0 + (lambda - 1.0) * zeta));
    }
    result[new_Mz - 1] = new_Lz;  // make sure it is exactly equal
    break;
  }
  default:
    throw RuntimeError("spacing can not be UNKNOWN");
  }

  return result;
}

//! Return the index `k` into `zlevels[]` so that `zlevels[k] <= height < zlevels[k+1]` and `k < Mz`.
unsigned int IceGrid::kBelowHeight(double height) const {

  if (height < 0.0 - 1.0e-6) {
    throw RuntimeError::formatted("height = %5.4f is below base of ice"
                                  " (height must be non-negative)\n", height);
  }

  if (height > Lz() + 1.0e-6) {
    throw RuntimeError::formatted("height = %5.4f is above top of computational"
                                  " grid Lz = %5.4f\n", height, Lz());
  }

  return gsl_interp_accel_find(m_impl->bsearch_accel, &m_impl->z[0], m_impl->z.size(), height);
}

//! \brief Computes the number of processors in the X- and Y-directions.
static void compute_nprocs(unsigned int Mx, unsigned int My, unsigned int size,
                           unsigned int &Nx, unsigned int &Ny) {

  if (My <= 0) {
    throw RuntimeError("'My' is invalid.");
  }

  Nx = (unsigned int)(0.5 + sqrt(((double)Mx)*((double)size)/((double)My)));
  Ny = 0;

  if (Nx == 0) {
    Nx = 1;
  }

  while (Nx > 0) {
    Ny = size / Nx;
    if (Nx*Ny == (unsigned int)size) {
      break;
    }
    Nx--;
  }

  if (Mx > My and Nx < Ny) {
    // Swap Nx and Ny
    int tmp = Nx;
    Nx = Ny;
    Ny = tmp;
  }

  if ((Mx / Nx) < 2) {          // note: integer division
    throw RuntimeError::formatted("Can't split %d grid points into %d parts (X-direction).",
                                  Mx, (int)Nx);
  }

  if ((My / Ny) < 2) {          // note: integer division
    throw RuntimeError::formatted("Can't split %d grid points into %d parts (Y-direction).",
                                  My, (int)Ny);
  }
}


//! \brief Computes processor ownership ranges corresponding to equal area
//! distribution among processors.
static std::vector<unsigned int> ownership_ranges(unsigned int Mx,
                                              unsigned int Nx) {

  std::vector<unsigned int> result(Nx);

  for (unsigned int i=0; i < Nx; i++) {
    result[i] = Mx / Nx + ((Mx % Nx) > i);
  }
  return result;
}

//! Set processor ownership ranges. Takes care of type conversion (`unsigned int` -> `PetscInt`).
void IceGrid::set_ownership_ranges(const std::vector<unsigned int> &procs_x,
                                   const std::vector<unsigned int> &procs_y) {
  if (procs_x.size() * procs_y.size() != size()) {
    throw RuntimeError("length(procs_x) * length(procs_y) != MPI size");
  }

  m_impl->procs_x.resize(procs_x.size());
  for (unsigned int k = 0; k < procs_x.size(); ++k) {
    m_impl->procs_x[k] = procs_x[k];
  }

  m_impl->procs_y.resize(procs_y.size());
  for (unsigned int k = 0; k < procs_y.size(); ++k) {
    m_impl->procs_y[k] = procs_y[k];
  }
}

struct OwnershipRanges {
  std::vector<unsigned int> x, y;
};

//! Compute processor ownership ranges using the grid size, MPI communicator size, and command-line
//! options `-Nx`, `-Ny`, `-procs_x`, `-procs_y`.
static OwnershipRanges compute_ownership_ranges(unsigned int Mx,
                                                unsigned int My,
                                                unsigned int size) {
  OwnershipRanges result;

  unsigned int Nx_default, Ny_default;
  compute_nprocs(Mx, My, size, Nx_default, Ny_default);

  // check -Nx and -Ny
  options::Integer Nx("-Nx", "Number of processors in the x direction", Nx_default);
  options::Integer Ny("-Ny", "Number of processors in the y direction", Ny_default);

  // validate results (compute_nprocs checks its results, but we also need to validate command-line
  // options)
  if ((Mx / Nx) < 2) {
    throw RuntimeError::formatted("Can't split %d grid points into %d parts (X-direction).",
                                  Mx, (int)Nx);
  }

  if ((My / Ny) < 2) {
    throw RuntimeError::formatted("Can't split %d grid points into %d parts (Y-direction).",
                                  My, (int)Ny);
  }

  if (Nx * Ny != (int)size) {
    throw RuntimeError::formatted("Nx * Ny has to be equal to %d.", size);
  }

  // check -procs_x and -procs_y
  options::IntegerList procs_x("-procs_x", "Processor ownership ranges (x direction)");
  options::IntegerList procs_y("-procs_y", "Processor ownership ranges (y direction)");

  if (procs_x.is_set()) {
    if (procs_x->size() != (unsigned int)Nx) {
      throw RuntimeError("-Nx has to be equal to the -procs_x size.");
    }

    result.x.resize(procs_x->size());
    for (unsigned int k = 0; k < procs_x->size(); ++k) {
      result.x[k] = procs_x[k];
    }

  } else {
    result.x = ownership_ranges(Mx, Nx);
  }

  if (procs_y.is_set()) {
    if (procs_y->size() != (unsigned int)Ny) {
      throw RuntimeError("-Ny has to be equal to the -procs_y size.");
    }

    result.y.resize(procs_y->size());
    for (unsigned int k = 0; k < procs_y->size(); ++k) {
      result.y[k] = procs_y[k];
    }
  } else {
    result.y = ownership_ranges(My, Ny);
  }

  if (result.x.size() * result.y.size() != size) {
    throw RuntimeError("length(procs_x) * length(procs_y) != MPI size");
  }

  return result;
}

//! Compute horizontal grid spacing. See compute_horizontal_coordinates() for more.
static double compute_horizontal_spacing(double half_width, unsigned int M, bool periodic) {
  if (periodic) {
    return 2.0 * half_width / M;
  } else {
    return 2.0 * half_width / (M - 1);
  }
}

//! Compute grid coordinates for one direction (X or Y).
static std::vector<double> compute_coordinates(unsigned int M, double delta,
                                               double v_min, double v_max,
                                               bool periodic) {
  std::vector<double> result(M);

  // Here v_min, v_max define the extent of the computational domain,
  // which is not necessarily the same thing as the smallest and
  // largest values of grid coordinates.
  if (periodic) {
    for (unsigned int i = 0; i < M; ++i) {
      result[i] = v_min + (i + 0.5) * delta;
    }
    result[M - 1] = v_max - 0.5*delta;
  } else {
    for (unsigned int i = 0; i < M; ++i) {
      result[i] = v_min + i * delta;
    }
    result[M - 1] = v_max;
  }
  return result;
}

//! Compute horizontal spacing parameters `dx` and `dy` and grid coordinates using `Mx`, `My`, `Lx`, `Ly` and periodicity.
/*!
The grid used in PISM, in particular the PETSc DAs used here, are periodic in x and y.
This means that the ghosted values ` foo[i+1][j], foo[i-1][j], foo[i][j+1], foo[i][j-1]`
for all 2D Vecs, and similarly in the x and y directions for 3D Vecs, are always available.
That is, they are available even if i,j is a point at the edge of the grid.  On the other
hand, by default, `dx`  is the full width  `2 * Lx`  divided by  `Mx - 1`.
This means that we conceive of the computational domain as starting at the `i = 0`
grid location and ending at the  `i = Mx - 1`  grid location, in particular.
This idea is not quite compatible with the periodic nature of the grid.

The upshot is that if one computes in a truly periodic way then the gap between the
`i = 0`  and  `i = Mx - 1`  grid points should \em also have width  `dx`.
Thus we compute  `dx = 2 * Lx / Mx`.
 */
void IceGrid::compute_horizontal_coordinates() {

  m_impl->dx = compute_horizontal_spacing(m_impl->Lx, m_impl->Mx,
                                          m_impl->periodicity & X_PERIODIC);

  m_impl->dy = compute_horizontal_spacing(m_impl->Ly, m_impl->My,
                                          m_impl->periodicity & Y_PERIODIC);

  double
    x_min = m_impl->x0 - m_impl->Lx,
    x_max = m_impl->x0 + m_impl->Lx;

  m_impl->x = compute_coordinates(m_impl->Mx, m_impl->dx,
                                  x_min, x_max,
                                  m_impl->periodicity & X_PERIODIC);

  double
    y_min = m_impl->y0 - m_impl->Ly,
    y_max = m_impl->y0 + m_impl->Ly;

  m_impl->y = compute_coordinates(m_impl->My, m_impl->dy,
                                  y_min, y_max,
                                  m_impl->periodicity & Y_PERIODIC);
}

//! \brief Report grid parameters.
void IceGrid::report_parameters() const {

  const Logger &log = *this->ctx()->log();
  units::System::Ptr sys = this->ctx()->unit_system();

  log.message(2, "computational domain and grid:\n");

  units::Converter km(sys, "m", "km");

  // report on grid
  log.message(2,
              "                grid size   %d x %d x %d\n",
              Mx(), My(), Mz());

  // report on computational box
  log.message(2,
              "           spatial domain   %.2f km x %.2f km x %.2f m\n",
              km(2*Lx()), km(2*Ly()), Lz());

  // report on grid cell dims
  log.message(2,
              "     horizontal grid cell   %.2f km x %.2f km\n",
              km(dx()), km(dy()));

  if (fabs(dz_max() - dz_min()) <= 1.0e-8) {
    log.message(2,
                "  vertical spacing in ice   dz = %.3f m (equal spacing)\n",
                dz_min());
  } else {
    log.message(2,
                "  vertical spacing in ice   uneven, %d levels, %.3f m < dz < %.3f m\n",
                Mz(), dz_min(), dz_max());
  }

  // if -verbose (=-verbose 3) then (somewhat redundantly) list parameters of grid
  {
    log.message(3,
                "  IceGrid parameters:\n");
    log.message(3,
                "            Lx = %6.2f km, Ly = %6.2f km, Lz = %6.2f m, \n",
                km(Lx()), km(Ly()), Lz());
    log.message(3,
                "            x0 = %6.2f km, y0 = %6.2f km, (coordinates of center)\n",
                km(x0()), km(y0()));
    log.message(3,
                "            Mx = %d, My = %d, Mz = %d, \n",
                Mx(), My(), Mz());
    log.message(3,
                "            dx = %6.3f km, dy = %6.3f km, \n",
                km(dx()), km(dy()));
    log.message(3,
                "            Nx = %d, Ny = %d\n",
                (int)m_impl->procs_x.size(), (int)m_impl->procs_y.size());

    log.message(3,
                "            Periodicity: %s\n",
                periodicity_to_string(m_impl->periodicity).c_str());
  }

  {
    log.message(5,
                "  REALLY verbose output on IceGrid:\n");
    log.message(5,
                "    vertical levels in ice (Mz=%d, Lz=%5.4f): ", Mz(), Lz());
    for (unsigned int k=0; k < Mz(); k++) {
      log.message(5, " %5.4f, ", z(k));
    }
    log.message(5, "\n");
  }

}


//! \brief Computes indices of grid points to the lower left and upper right from (X,Y).
/*!
 * \code
 * 3       2
 * o-------o
 * |       |
 * |    +  |
 * o-------o
 * 0       1
 * \endcode
 *
 * If "+" is the point (X,Y), then (i_left, j_bottom) corresponds to
 * point "0" and (i_right, j_top) corresponds to point "2".
 *
 * Does not check if the resulting indexes are in the current
 * processor's domain. Ensures that computed indexes are within the
 * grid.
 */
void IceGrid::compute_point_neighbors(double X, double Y,
                                      int &i_left, int &i_right,
                                      int &j_bottom, int &j_top) const {
  i_left = (int)floor((X - m_impl->x[0])/m_impl->dx);
  j_bottom = (int)floor((Y - m_impl->y[0])/m_impl->dy);

  i_right = i_left + 1;
  j_top = j_bottom + 1;

  if (i_left < 0) {
    i_left = i_right;
  }

  if (i_right > (int)m_impl->Mx - 1) {
    i_right = i_left;
  }

  if (j_bottom < 0) {
    j_bottom = j_top;
  }

  if (j_top > (int)m_impl->My - 1) {
    j_top = j_bottom;
  }
}

//! \brief Compute 4 interpolation weights necessary for linear interpolation
//! from the current grid. See compute_point_neighbors for the ordering of
//! neighbors.
std::vector<double> IceGrid::compute_interp_weights(double X, double Y) const{
  int i_left = 0, i_right = 0, j_bottom = 0, j_top = 0;
  // these values (zeros) are used when interpolation is impossible
  double alpha = 0.0, beta = 0.0;

  compute_point_neighbors(X, Y, i_left, i_right, j_bottom, j_top);

  if (i_left != i_right) {
    assert(m_impl->x[i_right] - m_impl->x[i_left] != 0.0);
    alpha = (X - m_impl->x[i_left]) / (m_impl->x[i_right] - m_impl->x[i_left]);
  }

  if (j_bottom != j_top) {
    assert(m_impl->y[j_top] - m_impl->y[j_bottom] != 0.0);
    beta  = (Y - m_impl->y[j_bottom]) / (m_impl->y[j_top] - m_impl->y[j_bottom]);
  }

  std::vector<double> result(4);
  result[0] = alpha * beta;
  result[1] = (1 - alpha) * beta;
  result[2] = (1 - alpha) * (1 - beta);
  result[3] = alpha * (1 - beta);

  return result;
}

// Computes the key corresponding to the DM with given dof and stencil_width.
static int grid_dm_key(int da_dof, int stencil_width) {
  return 10000 * da_dof + stencil_width;
}

//! @brief Get a PETSc DM ("distributed array manager") object for given `dof` (number of degrees of
//! freedom per grid point) and stencil width.
petsc::DM::Ptr IceGrid::get_dm(int da_dof, int stencil_width) const {
  petsc::DM::Ptr result;

  if (da_dof < 0 or da_dof > 10000) {
    throw RuntimeError::formatted("Invalid da_dof argument: %d", da_dof);
  }

  if (stencil_width < 0 or stencil_width > 10000) {
    throw RuntimeError::formatted("Invalid stencil_width argument: %d", stencil_width);
  }

  int j = grid_dm_key(da_dof, stencil_width);

  if (m_impl->dms[j].expired()) {
    result = this->create_dm(da_dof, stencil_width);
    m_impl->dms[j] = result;
  } else {
    result = m_impl->dms[j].lock();
  }

  return result;
}

//! Return grid periodicity.
Periodicity IceGrid::periodicity() const {
  return m_impl->periodicity;
}

//! Return execution context this grid corresponds to.
Context::ConstPtr IceGrid::ctx() const {
  return m_impl->ctx;
}

//! @brief Create a DM with the given number of `dof` (degrees of freedom per grid point) and
//! stencil width.
petsc::DM::Ptr IceGrid::create_dm(int da_dof, int stencil_width) const {

  ctx()->log()->message(3,
             "* Creating a DM with dof=%d and stencil_width=%d...\n",
             da_dof, stencil_width);

  DM result;
  PetscErrorCode ierr = DMDACreate2d(com,
#if PETSC_VERSION_LT(3,5,0)
                                     DMDA_BOUNDARY_PERIODIC, DMDA_BOUNDARY_PERIODIC,
#else
                                     DM_BOUNDARY_PERIODIC, DM_BOUNDARY_PERIODIC,
#endif
                                     DMDA_STENCIL_BOX,
                                     m_impl->My, m_impl->Mx, // N, M
                                     (PetscInt)m_impl->procs_y.size(),
                                     (PetscInt)m_impl->procs_x.size(), // n, m
                                     da_dof, stencil_width,
                                     &m_impl->procs_y[0], &m_impl->procs_x[0], // ly, lx
                                     &result);
  PISM_CHK(ierr,"DMDACreate2d");

  return petsc::DM::Ptr(new petsc::DM(result));
}

//! MPI rank.
int IceGrid::rank() const {
  return m_impl->rank;
}

//! MPI communicator size.
unsigned int IceGrid::size() const {
  return m_impl->size;
}

//! Dictionary of variables (2D and 3D fields) associated with this grid.
Vars& IceGrid::variables() {
  return m_impl->variables;
}

//! Dictionary of variables (2D and 3D fields) associated with this grid.
const Vars& IceGrid::variables() const {
  return m_impl->variables;
}

//! Global starting index of this processor's subset.
int IceGrid::xs() const {
  return m_impl->xs;
}

//! Global starting index of this processor's subset.
int IceGrid::ys() const {
  return m_impl->ys;
}

//! Width of this processor's sub-domain.
int IceGrid::xm() const {
  return m_impl->xm;
}

//! Width of this processor's sub-domain.
int IceGrid::ym() const {
  return m_impl->ym;
}

//! Total grid size in the X direction.
unsigned int IceGrid::Mx() const {
  return m_impl->Mx;
}

//! Total grid size in the Y direction.
unsigned int IceGrid::My() const {
  return m_impl->My;
}

//! Number of vertical levels.
unsigned int IceGrid::Mz() const {
  return m_impl->z.size();
}

//! X-coordinates.
const std::vector<double>& IceGrid::x() const {
  return m_impl->x;
}

//! Get a particular x coordinate.
double IceGrid::x(size_t i) const {
  return m_impl->x[i];
}

//! Y-coordinates.
const std::vector<double>& IceGrid::y() const {
  return m_impl->y;
}

//! Get a particular y coordinate.
double IceGrid::y(size_t i) const {
  return m_impl->y[i];
}

//! Z-coordinates within the ice.
const std::vector<double>& IceGrid::z() const {
  return m_impl->z;
}

//! Get a particular z coordinate.
double IceGrid::z(size_t i) const {
  return m_impl->z[i];
}

//! Horizontal grid spacing.
double IceGrid::dx() const {
  return m_impl->dx;
}

//! Horizontal grid spacing.
double IceGrid::dy() const {
  return m_impl->dy;
}

//! Minimum vertical spacing.
double IceGrid::dz_min() const {
  double result = m_impl->z.back();
  for (unsigned int k = 0; k < m_impl->z.size() - 1; ++k) {
    const double dz = m_impl->z[k + 1] - m_impl->z[k];
    result = std::min(dz, result);
  }
  return result;
}

//! Maximum vertical spacing.
double IceGrid::dz_max() const {
  double result = 0.0;
  for (unsigned int k = 0; k < m_impl->z.size() - 1; ++k) {
    const double dz = m_impl->z[k + 1] - m_impl->z[k];
    result = std::max(dz, result);
  }
  return result;
}

//! Half-width of the computational domain.
double IceGrid::Lx() const {
  return m_impl->Lx;
}

//! Half-width of the computational domain.
double IceGrid::Ly() const {
  return m_impl->Ly;
}

//! Height of the computational domain.
double IceGrid::Lz() const {
  return m_impl->z.back();
}

//! X-coordinate of the center of the domain.
double IceGrid::x0() const {
  return m_impl->x0;
}

//! Y-coordinate of the center of the domain.
double IceGrid::y0() const {
  return m_impl->y0;
}

//! @brief Returns the distance from the point (i,j) to the origin.
double radius(const IceGrid &grid, int i, int j) {
  return sqrt(grid.x(i) * grid.x(i) + grid.y(j) * grid.y(j));
}

// grid_info

void grid_info::reset() {

  t_len = 0;
  time  = 0;

  x_len = 0;
  x0    = 0;
  Lx    = 0;

  y_len = 0;
  y0    = 0;
  Ly    = 0;

  z_len = 0;
  z_min = 0;
  z_max = 0;
}

grid_info::grid_info() {
  reset();
}

void grid_info::report(const Logger &log, int threshold, units::System::Ptr s) const {
  units::Converter km(s, "m", "km");

  log.message(threshold,
              "  x:  %5d points, [%10.3f, %10.3f] km, x0 = %10.3f km, Lx = %10.3f km\n",
              this->x_len,
              km(this->x0 - this->Lx),
              km(this->x0 + this->Lx),
              km(this->x0),
              km(this->Lx));

  log.message(threshold,
              "  y:  %5d points, [%10.3f, %10.3f] km, y0 = %10.3f km, Ly = %10.3f km\n",
              this->y_len,
              km(this->y0 - this->Ly),
              km(this->y0 + this->Ly),
              km(this->y0),
              km(this->Ly));

  log.message(threshold,
              "  z:  %5d points, [%10.3f, %10.3f] m\n",
              this->z_len, this->z_min, this->z_max);

  log.message(threshold,
              "  t:  %5d points, last time = %.3f years\n\n",
              this->t_len, units::convert(s, this->time, "seconds", "years"));
}

grid_info::grid_info(const PIO &file, const std::string &variable,
                     units::System::Ptr unit_system,
                     Periodicity p) {
  try {
    bool variable_exists, found_by_standard_name;
    std::string name_found;

    reset();

    // try "variable" as the standard_name first, then as the short name:
    file.inq_var(variable, variable, variable_exists,
                 name_found, found_by_standard_name);

    if (not variable_exists) {
      throw RuntimeError::formatted("variable \"%s\" is missing", variable.c_str());
    }

    std::vector<std::string> dims = file.inq_vardims(name_found);

    // use "global" dimensions (as opposed to dimensions of a patch)
    if (file.backend_type() == "quilt") {
      for (unsigned int i = 0; i < dims.size(); ++i) {
        if (dims[i] == "x_patch") {
          dims[i] = "x";
        }

        if (dims[i] == "y_patch") {
          dims[i] = "y";
        }
      }
    }

    for (unsigned int i = 0; i < dims.size(); ++i) {
      std::string dimname = dims[i];

      AxisType dimtype = file.inq_dimtype(dimname, unit_system);

      switch (dimtype) {
      case X_AXIS:
        {
          double x_min = 0.0, x_max = 0.0;
          file.inq_dim_limits(dimname, &x_min, &x_max);
          file.get_dim(dimname, this->x);
          this->x_len = this->x.size();
          this->x0 = 0.5 * (x_min + x_max);
          this->Lx = 0.5 * (x_max - x_min);
          if (p & X_PERIODIC) {
            const double dx = this->x[1] - this->x[0];
            this->Lx += 0.5 * dx;
          }
          break;
        }
      case Y_AXIS:
        {
          double y_min = 0.0, y_max = 0.0;
          file.inq_dim_limits(dimname, &y_min, &y_max);
          file.get_dim(dimname, this->y);
          this->y_len = this->y.size();
          this->y0 = 0.5 * (y_min + y_max);
          this->Ly = 0.5 * (y_max - y_min);
          if (p & Y_PERIODIC) {
            const double dy = this->y[1] - this->y[0];
            this->Ly += 0.5 * dy;
          }
          break;
        }
      case Z_AXIS:
        {
          file.inq_dim_limits(dimname, &this->z_min, &this->z_max);
          file.get_dim(dimname, this->z);
          this->z_len = this->z.size();
          break;
        }
      case T_AXIS:
        {
          this->t_len = file.inq_dimlen(dimname);
          file.inq_dim_limits(dimname, NULL, &this->time);
          break;
        }
      default:
        {
          throw RuntimeError::formatted("can't figure out which direction dimension '%s' corresponds to.",
                                        dimname.c_str());
        }
      } // switch
    }   // for loop
  } catch (RuntimeError &e) {
    e.add_context("getting grid information using variable '%s' in '%s'", variable.c_str(),
                  file.inq_filename().c_str());
    throw;
  }
}

GridParameters::GridParameters() {

  // set to something invalid
  Lx = -1.0;
  Ly = -1.0;

  x0 = 0.0;
  y0 = 0.0;

  Mx = 0;
  My = 0;

  periodicity = NONE;
}

GridParameters::GridParameters(Config::ConstPtr config) {
  init_from_config(config);
}

void GridParameters::ownership_ranges_from_options(unsigned int size) {
  OwnershipRanges procs = compute_ownership_ranges(Mx, My, size);
  procs_x = procs.x;
  procs_y = procs.y;
}

//! Initialize from a configuration database. Does not try to compute ownership ranges.
void GridParameters::init_from_config(Config::ConstPtr config) {
  Lx = config->get_double("grid_Lx");
  Ly = config->get_double("grid_Ly");

  x0 = 0.0;
  y0 = 0.0;

  Mx = config->get_double("grid_Mx");
  My = config->get_double("grid_My");

  periodicity = string_to_periodicity(config->get_string("grid_periodicity"));

  double Lz = config->get_double("grid_Lz");
  unsigned int Mz = config->get_double("grid_Mz");
  double lambda = config->get_double("grid_lambda");
  SpacingType s = string_to_spacing(config->get_string("grid_ice_vertical_spacing"));
  z = IceGrid::compute_vertical_levels(Lz, Mz, s, lambda);
  // does not set ownership ranges because we don't know if these settings are final
}

void GridParameters::init_from_file(Context::Ptr ctx,
                                    const PIO &file,
                                    const std::string &variable_name,
                                    Periodicity p) {
  int size = 0;
  MPI_Comm_size(ctx->com(), &size);

  // set defaults (except for ownership ranges) from configuration parameters
  init_from_config(ctx->config());

  grid_info input_grid(file, variable_name, ctx->unit_system(), p);

  Lx = input_grid.Lx;
  Ly = input_grid.Ly;
  x0 = input_grid.x0;
  y0 = input_grid.y0;
  Mx = input_grid.x_len;
  My = input_grid.y_len;
  periodicity = p;
  z = input_grid.z;
}

GridParameters::GridParameters(Context::Ptr ctx,
                               const PIO &file,
                               const std::string &variable_name,
                               Periodicity p) {
  init_from_file(ctx, file, variable_name, p);
}

GridParameters::GridParameters(Context::Ptr ctx,
                               const std::string &filename,
                               const std::string &variable_name,
                               Periodicity p) {
  PIO nc(ctx->com(), "netcdf3");
  nc.open(filename, PISM_READONLY);
  init_from_file(ctx, nc, variable_name, p);
  nc.close();
}


void GridParameters::horizontal_size_from_options() {
  Mx = options::Integer("-Mx", "grid size in X direction", Mx);
  My = options::Integer("-My", "grid size in Y direction", My);
}

void GridParameters::horizontal_extent_from_options() {
  // Domain size
  {
    Lx = 1000.0 * options::Real("-Lx", "Half of the grid extent in the Y direction, in km",
                                Lx / 1000.0);
    Ly = 1000.0 * options::Real("-Ly", "Half of the grid extent in the X direction, in km",
                                Ly / 1000.0);
  }

  // Alternatively: domain size and extent
  {
    options::RealList x_range("-x_range", "min,max x coordinate values");
    options::RealList y_range("-y_range", "min,max y coordinate values");

    if (x_range.is_set() and y_range.is_set()) {
      if (x_range->size() != 2 or y_range->size() != 2) {
        throw RuntimeError("-x_range and/or -y_range argument is invalid.");
      }
      x0 = (x_range[0] + x_range[1]) / 2.0;
      y0 = (y_range[0] + y_range[1]) / 2.0;
      Lx = (x_range[1] - x_range[0]) / 2.0;
      Ly = (y_range[1] - y_range[0]) / 2.0;
    }
  }
}

void GridParameters::vertical_grid_from_options(Config::ConstPtr config) {
  options::Real Lz("-Lz", "height of the computational domain",
                   z.size() > 0 ? z.back() : 0.0);
  options::Integer Mz("-Mz", "grid size in Y direction", z.size());

  double lambda = config->get_double("grid_lambda");
  SpacingType s = string_to_spacing(config->get_string("grid_ice_vertical_spacing"));
  z = IceGrid::compute_vertical_levels(Lz, Mz, s, lambda);
}

void GridParameters::validate() const {
  if (Mx < 3) {
    throw RuntimeError::formatted("Mx = %d is invalid (has to be 3 or greater)", Mx);
  }

  if (My < 3) {
    throw RuntimeError::formatted("My = %d is invalid (has to be 3 or greater)", My);
  }

  if (Lx <= 0.0) {
    throw RuntimeError::formatted("Lx = %f is invalid (negative)", Lx);
  }

  if (Ly <= 0.0) {
    throw RuntimeError::formatted("Ly = %f is invalid (negative)", Ly);
  }

  if (not is_increasing(z)) {
    throw RuntimeError("z levels are not increasing");
  }

  if (z[0] > 1e-6) {
    throw RuntimeError::formatted("first z level is not zero: %f", z[0]);
  }

  if (z.back() < 0.0) {
    throw RuntimeError::formatted("last z level is negative: %f", z.back());
  }

  unsigned int sum = 0;
  for (unsigned int k = 0; k < procs_x.size(); ++k) {
    sum += procs_x[k];
  }

  if (sum != Mx) {
    throw RuntimeError("procs_x don't sum up to Mx");
  }

  sum = 0;
  for (unsigned int k = 0; k < procs_y.size(); ++k) {
    sum += procs_y[k];
  }

  if (sum != My) {
    throw RuntimeError("procs_y don't sum up to My");
  }
}

//! Create a grid using command-line options and (possibly) an input file.
/** Processes options -i, -bootstrap, -Mx, -My, -Mz, -Lx, -Ly, -Lz, -x_range, -y_range.
 */
IceGrid::Ptr IceGrid::FromOptions(Context::Ptr ctx) {
  options::String input_file("-i", "Specifies a PISM input file");
  bool bootstrap = options::Bool("-bootstrap", "enable bootstrapping heuristics");

  Periodicity p = string_to_periodicity(ctx->config()->get_string("grid_periodicity"));

  if (input_file.is_set() and (not bootstrap)) {
    // These options are ignored because we're getting *all* the grid
    // parameters from a file.
    options::ignored(*ctx->log(), "-Mx");
    options::ignored(*ctx->log(), "-My");
    options::ignored(*ctx->log(), "-Mz");
    options::ignored(*ctx->log(), "-Mbz");
    options::ignored(*ctx->log(), "-Lx");
    options::ignored(*ctx->log(), "-Ly");
    options::ignored(*ctx->log(), "-Lz");
    options::ignored(*ctx->log(), "-z_spacing");

    // get grid from a PISM input file
    std::vector<std::string> names;
    names.push_back("enthalpy");
    names.push_back("temp");

    return IceGrid::FromFile(ctx, input_file, names, p);
  } else if (input_file.is_set() and bootstrap) {
    // bootstrapping; get domain size defaults from an input file, allow overriding all grid
    // parameters using command-line options

    GridParameters input_grid(ctx->config());

    std::vector<std::string> names;
    names.push_back("land_ice_thickness");
    names.push_back("bedrock_altitude");
    names.push_back("thk");
    names.push_back("topg");
    bool grid_info_found = false;

    PIO nc(ctx->com(), "netcdf3");
    nc.open(input_file, PISM_READONLY); // will be closed automatically
    for (unsigned int i = 0; i < names.size(); ++i) {

      grid_info_found = nc.inq_var(names[i]);
      if (not grid_info_found) {
        // Failed to find using a short name. Try using names[i] as a
        // standard name...
        std::string dummy1;
        bool dummy2;
        nc.inq_var("dummy", names[i], grid_info_found, dummy1, dummy2);
      }

      if (grid_info_found) {
        input_grid = GridParameters(ctx, nc, names[i], p);
        break;
      }
    }

    if (not grid_info_found) {
      throw RuntimeError::formatted("no geometry information found in '%s'",
                                    input_file->c_str());
    }

    // process all possible options controlling grid parameters, overriding values read from a file
    input_grid.horizontal_size_from_options();
    input_grid.horizontal_extent_from_options();
    input_grid.vertical_grid_from_options(ctx->config());
    input_grid.ownership_ranges_from_options(ctx->size());

    return IceGrid::Ptr(new IceGrid(ctx, input_grid));
  } else {
    // This covers the two remaining cases "-i is not set, -bootstrap is set" and "-i is not set,
    // -bootstrap is not set either".
    throw RuntimeError("Please set the input file using the \"-i\" command-line option.");
  }
}

} // end of namespace pism
