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

#include "PISMIcebergRemover.hh"
#include "connected_components.hh"
#include "base/util/Mask.hh"
#include "base/util/PISMVars.hh"
#include "base/util/error_handling.hh"
#include "base/util/IceGrid.hh"

namespace pism {
namespace calving {

IcebergRemover::IcebergRemover(IceGrid::ConstPtr g)
  : Component(g) {

  m_iceberg_mask.create(m_grid, "iceberg_mask", WITHOUT_GHOSTS);
  m_mask_p0 = m_iceberg_mask.allocate_proc0_copy();
}

IcebergRemover::~IcebergRemover() {
  // empty
}

void IcebergRemover::init() {
}

/**
 * Use PISM's ice cover mask to update ice thickness, removing "icebergs".
 *
 * @param[in,out] pism_mask PISM's ice cover mask
 * @param[in,out] ice_thickness ice thickness
 */
void IcebergRemover::update(IceModelVec2Int &pism_mask,
                            IceModelVec2S &ice_thickness) {
  const int
    mask_grounded_ice = 1,
    mask_floating_ice = 2;
  MaskQuery M(pism_mask);

  // prepare the mask that will be handed to the connected component
  // labeling code:
  {
    m_iceberg_mask.set(0.0);

    IceModelVec::AccessList list;
    list.add(pism_mask);
    list.add(m_iceberg_mask);

    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();

      if (M.grounded_ice(i,j) == true) {
        m_iceberg_mask(i,j) = mask_grounded_ice;
      } else if (M.floating_ice(i,j) == true) {
        m_iceberg_mask(i,j) = mask_floating_ice;
      }
    }

    // Mark icy SSA Dirichlet B.C. cells as "grounded" because we
    // don't want them removed.
    if (m_grid->variables().is_available("bc_mask")) {
      const IceModelVec2Int &bc_mask = *m_grid->variables().get_2d_mask("bc_mask");
      list.add(bc_mask);

      for (Points p(*m_grid); p; p.next()) {
        const int i = p.i(), j = p.j();

        if (bc_mask(i,j) > 0.5 and M.icy(i,j)) {
          m_iceberg_mask(i,j) = mask_grounded_ice;
        }
      }
    }
  }

  // identify icebergs using serial code on processor 0:
  {
    m_iceberg_mask.put_on_proc0(*m_mask_p0);

    ParallelSection rank0(m_grid->com);
    try {
      if (m_grid->rank() == 0) {
        petsc::VecArray mask(*m_mask_p0);
        cc(mask.get(), m_grid->Mx(), m_grid->My(), true, mask_grounded_ice);
      }
    } catch (...) {
      rank0.failed();
    }
    rank0.check();

    m_iceberg_mask.get_from_proc0(*m_mask_p0);
  }

  // correct ice thickness and the cell type mask using the resulting
  // "iceberg" mask:
  {
    IceModelVec::AccessList list;
    list.add(ice_thickness);
    list.add(pism_mask);
    list.add(m_iceberg_mask);

    if (m_grid->variables().is_available("bc_mask")) {
      const IceModelVec2Int &bc_mask = *m_grid->variables().get_2d_mask("bc_mask");
      // if SSA Dirichlet B.C. are in use, do not modify mask and ice
      // thickness at Dirichlet B.C. locations
      list.add(bc_mask);

      for (Points p(*m_grid); p; p.next()) {
        const int i = p.i(), j = p.j();

        if (m_iceberg_mask(i,j) > 0.5 && bc_mask(i,j) < 0.5) {
          ice_thickness(i,j) = 0.0;
          pism_mask(i,j)     = MASK_ICE_FREE_OCEAN;
        }
      }
    } else {

      for (Points p(*m_grid); p; p.next()) {
        const int i = p.i(), j = p.j();

        if (m_iceberg_mask(i,j) > 0.5) {
          ice_thickness(i,j) = 0.0;
          pism_mask(i,j)     = MASK_ICE_FREE_OCEAN;
        }
      }
    }
  }

  // update ghosts of the mask and the ice thickness (then surface
  // elevation can be updated redundantly)
  pism_mask.update_ghosts();
  ice_thickness.update_ghosts();
}

void IcebergRemover::add_vars_to_output_impl(const std::string &, std::set<std::string> &) {
  // empty
}

void IcebergRemover::define_variables_impl(const std::set<std::string> &, const PIO &,
                                                IO_Type) {
  // empty
}

void IcebergRemover::write_variables_impl(const std::set<std::string> &, const PIO&) {
  // empty
}

} // end of namespace calving
} // end of namespace pism
