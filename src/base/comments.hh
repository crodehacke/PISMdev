// THE ONLY PURPOSE OF THIS FILE IS TO BE A LOCATION FOR MAJOR SOURCE CODE COMMENTS.
// IT NEEDS TO BE A .hh (OR OTHER STANDARD SOURCE TYPE) AND IT NEEDS TO BE IN THE
// pism/src/ TREE SO THAT doxygen WILL AUTOMATICALLY READ IT.


// add commentary and GPL to first page of doxygen-erated HTML source browser
/*! @cond BROWSER \mainpage 
 *
 * This source browser shows the C++ class (object) structure of PISM.  It 
 * shows \em all the classes in PISM, \em all the inheritance structure and
 * \em all the members (variables and methods) of all the PISM classes.  
 * It was automatically generated by doxygen (http://www.doxygen.org/)
 * from comments in the PISM source code.
 *
 * See the PISM User's Manual (http://www.dms.uaf.edu/~bueler/manual.pdf or 
 * <tt>pism/doc/manual.pdf</tt>) for help with installing and using PISM.  
 * Most users should stick to the User's Manual for quite a while; only when 
 * a user needs to extend PISM is a look at this source browser really 
 * worthwhile.
 *
 * See the PISM Reference Manual (http://www.dms.uaf.edu/~bueler/refman.pdf or 
 * <tt>pism/doc/refman.pdf</tt>) for a useful subset of this source browser
 * in PDF form.  The Reference Manual contains the minimum documentation 
 * of the PISM class structure in order to include all the important documentation 
 * of the continuum models and numerical methods of PISM.  (Such continuum model
 * and numerical method documentation is widely scattered in this source browser.)
 *
 * There is also a source code browsing tool at the PISM download site
 * (https://gna.org/projects/pism/).  That tool is <i>very different from this one</i>.  This one
 * documents the C++ class structure of the current revision.  The one at gna.org
 * shows changes between revisions but it ignors the class structure as it only cares 
 * about source code files as text files and generally ignors their semantics.
 *
 * <i>
 * Copyright (C) 2007 Ed Bueler
 *
 * This document is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
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
 * </i>
 * @endcond
 */
 

// add commentary and GPL to first page of doxygen-erated PDF Reference Manual
/*! @cond REFMAN \mainpage
 * 
 * \author Ed Bueler
 *
 * This Reference Manual is a greatly shortened version of the complete HTML
 * PISM source browser at <tt>pism/doc/doxy/html/index.html</tt>.  As with the source 
 * browser, this Manual was automatically generated by doxygen (http://www.doxygen.org/)
 * from comments in the PISM source code.
 *
 * This Manual documents the \em continuum \em models and the \em numerical
 * \em methods in PISM.  It is restricted to documenting the three classes MaterialType
 * (and its derived classes), IceGrid (and its associated class IceParam), and the main
 * PISM class IceModel.  The source files from which this Manual is drawn are limited to 
 * files in the <tt>pism/src/base/</tt> subdirectory.  Note that, by contrast, the source 
 * browser describes \em all classes and 
 * \em all class members and \em all source files in PISM.
 *
 * Most users should start with the PISM User's Manual 
 * (http://www.dms.uaf.edu/~bueler/manual.pdf or <tt>pism/doc/manual.pdf</tt>) for help
 * with installing and using PISM.
 *
 * <i>
 * Copyright (C) 2007 Ed Bueler
 *
 * This document is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
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
 * </i>
 * @endcond
 */


/*! \fn PetscErrorCode IceModel::temperatureStep()
    \brief Takes a semi-implicit time-step for the temperature equation.
    
@cond CONTINUUM
In summary, the conservation of energy equation is
    \f[ \rho c_p \frac{dT}{dt} = k \frac{\partial^2 T}{\partial z^2} + \Sigma,\f] 
where \f$T(t,x,y,z)\f$ is the temperature of the ice.  This equation is the shallow approximation
of the full three-dimensional conservation of energy.  Note \f$dT/dt\f$ stands for the material
derivative, so advection is included.  Here \f$\rho\f$ is the density of ice, 
\f$c_p\f$ is its specific heat, and \f$k\f$ is its conductivity.  Also \f$\Sigma\f$ is the volume
strain heating.
@endcond

@cond NUMERIC
In summary, the numerical method is first-order upwind for advection and centered-differences with
semi-implicitness for the vertical conduction term.  Note that we work from the bottom 
of the column upward in building the system to solve (in the semi-implicit time-stepping scheme).
Note that the excess energy above pressure melting is converted to melt-water, and that a fraction 
of this melt water is transported to the base according to the scheme in excessToFromBasalMeltLayer().
@endcond

@cond REFMAN
Here is a more complete discussion and derivation.

Consider a column of a slowly flowing and heat conducting material as shown in the left side 
of the next figure.  (The left side shows a general column of flowing and heat conduction 
material showing a small segment \f$V\f$.  The right side shows a more specific column of ice 
flowing and sliding over bedrock.)  This is an \em Eulerian view so the material flows through 
a column which remains fixed (and is notional).  The column is vertical.  We will 
assume when needed that it is rectangular in cross-section with cross-sectional area 
\f$\Delta x\Delta y\f$.

\image latex earlycols.png "Left: a general column of material.  Right: ice over bedrock." width=3in

[FIXME: CONTINUE TO MINE eqns3D.tex FOR MORE]
@endcond
 */



