// Copyright (C) 2011, 2012, 2013, 2014, 2015 PISM Authors
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

#ifndef _PSFORCETHICKNESS_H_
#define _PSFORCETHICKNESS_H_

#include "PSModifier.hh"
#include "base/util/iceModelVec.hh"
#include "base/util/VariableMetadata.hh"

namespace pism {
namespace surface {

//! A class implementing a modified surface mass balance which forces
//! ice thickness to a given target by the end of the run.
class ForceThickness : public SurfaceModifier {
public:
  ForceThickness(IceGrid::ConstPtr g, SurfaceModel *input);
  virtual ~ForceThickness();
protected:
  virtual void init_impl();
  virtual void ice_surface_mass_flux_impl(IceModelVec2S &result);
  virtual void ice_surface_temperature_impl(IceModelVec2S &result);
  virtual MaxTimestep max_timestep_impl(double my_t);
  virtual void write_variables_impl(const std::set<std::string> &vars, const PIO &nc);
  virtual void add_vars_to_output_impl(const std::string &keyword, std::set<std::string> &result);
  virtual void define_variables_impl(const std::set<std::string> &vars,
                                     const PIO &nc, IO_Type nctype);
private:
  std::string m_input_file;
  double m_alpha, m_alpha_ice_free_factor,  m_ice_free_thickness_threshold;
  IceModelVec2S m_target_thickness, m_ftt_mask;
  SpatialVariableMetadata m_climatic_mass_balance, m_climatic_mass_balance_original, m_ice_surface_temp;
};

} // end of namespace surface
} // end of namespace pism

#endif /* _PSFORCETHICKNESS_H_ */
