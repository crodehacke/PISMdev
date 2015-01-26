/* Copyright (C) 2015 PISM Authors
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

#include "DM.hh"

namespace pism {

PISMDM::PISMDM(DM dm) {
  m_value = dm;
}

PISMDM::PISMDM() {
  m_value = NULL;
}

PISMDM::~PISMDM() {
  if (m_value != NULL) {
    PetscErrorCode ierr = DMDestroy(&m_value); CHKERRCONTINUE(ierr);
  }
}

} // end of namespace pism
