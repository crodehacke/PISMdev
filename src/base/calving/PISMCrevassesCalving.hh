/* Copyright (C) 2013, 2014, 2015 PISM Authors
 *
 * This file is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * PISM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PISM; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef _PISMCREVASSESCALVING_H_
#define _PISMCREVASSESCALVING_H_

#include "base/util/iceModelVec.hh"
#include "base/util/PISMComponent.hh"
#include "coupler/PISMOcean.hh" //ccr

namespace pism {
namespace stressbalance {
class StressBalance;
}

namespace calving {

class CrevassesCalving : public Component
{
public:
  CrevassesCalving(IceGrid::ConstPtr g, stressbalance::StressBalance *stress_balance);
  virtual ~CrevassesCalving();

  virtual void init();
  void update(double dt,
              IceModelVec2Int &pism_mask,
              IceModelVec2S &Href,
              IceModelVec2S &ice_thickness,
	      IceModelVec2S &surface_h,
	      IceModelVec2S &bottom_h,
	      IceModelVec2S &crevasse_dw,
	      IceModelVec2S &crevasse_flux_2D,
	      const double sea_level);
              //ccr-future: IceModelVec2S &crevasse_dwdt,
              //ccr-future: const IceModelVec2S &smb);

  MaxTimestep max_timestep(const double sea_level);

  // empty methods that we're required to implement:
protected:
  virtual void write_variables_impl(const std::set<std::string> &vars, const PIO& nc);
  virtual void add_vars_to_output_impl(const std::string &keyword, std::set<std::string> &result);
  virtual void define_variables_impl(const std::set<std::string> &vars, const PIO &nc,
                                     IO_Type nctype);
  void update_strain_rates();
  void update_water_table_crevasses(IceModelVec2S &crevasse_dw_input,
				    IceModelVec2S &crevasse_dw_return);
                                    //ccr-future: IceModelVec2S &crevasse_dwdt,
                                    //ccr-future: const IceModelVec2S &smb);
  void update_crevasses_depths(IceModelVec2S &surface_h,
			       IceModelVec2S &bottom_h,
			       const IceModelVec2Int &pism_mask,
			       const IceModelVec2S &Href,
			       const IceModelVec2S &ice_thickness,
			       const IceModelVec2S &bed,
			       const IceModelVec3 &T3,
			       const IceModelVec2S &crevasse_dw,
			       const double sea_level);
  void remove_narrow_tongues(IceModelVec2Int &pism_mask, IceModelVec2S &ice_thickness);
protected:
  IceModelVec2 m_strain_rates;
  IceModelVec2S m_thk_loss, m_total_h, m_crevasse_dw; //ccr-future: m_crevasse_dwdt;
  IceModelVec2S m_surface_h, m_bottom_h; //Only for CrevassesCalving::max_timestep
  const int m_stencil_width;
  stressbalance::StressBalance *m_stress_balance;
  ocean::OceanModel *m_ocean;
  double m_dw, m_dw0, m_ice_density;
  bool m_restrict_timestep;
};

} // end of namespace calving
} // end of namespace pism

#endif /* _PISMCREVASSESCALVING_H_ */
