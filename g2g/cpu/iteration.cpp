/* -*- mode: c -*- */
#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include "../common.h"
#include "../init.h"
#include "../cuda/cuda_extra.h"
#include "../matrix.h"
#include "../timer.h"
#include "../partition.h"

#include "cpu/pot.h"

using std::cout;
using std::endl;
using std::list;
using namespace G2G;

template <bool compute_rmm, bool lda, bool compute_forces>
void g2g_iteration(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr)
{
  double total_energy = 0;

  Timer t_total, t_ciclos, t_rmm, t_density, t_forces, t_resto, t_pot, t_functions;
  t_total.start();

  HostMatrixFloat rmm_output;

  /********** iterate all groups ***********/
	for (list<PointGroup*>::const_iterator group_it = partition.group_list.begin(); group_it != partition.group_list.end(); ++group_it)
  {
    PointGroup& group = *(*group_it);
    uint group_m = group.total_functions();
    if (compute_rmm) { rmm_output.resize(group_m, group_m); rmm_output.zero(); }

    #if CPU_RECOMPUTE
    /** Compute this group's functions **/
    t_functions.start();
    group.compute_functions<compute_forces, !lda>();
    t_functions.pause();
    #endif

    // prepare rmm_input for this group
    t_density.start();
    HostMatrixFloat rmm_input(group_m, group_m);
    group.get_rmm_input(rmm_input);
    t_density.pause();

    HostMatrixCFloat3 forces(group.total_nucleii(), 1); forces.zero();
    HostMatrixCFloat3 dd;

    /******** each point *******/
    uint point = 0;
    for (list<Point>::const_iterator point_it = group.points.begin(); point_it != group.points.end(); ++point_it, ++point)
    {
      t_density.start();

      /** density **/
      float partial_density = 0;
      cfloat3 dxyz(0,0,0);
      cfloat3 dd1(0,0,0);
      cfloat3 dd2(0,0,0);

      if (lda) {
        for (uint i = 0; i < group_m; i++) {
          float w = 0.0f;
          float Fi = group.function_values(i, point);
          for (uint j = i; j < group_m; j++) {
            float Fj = group.function_values(j, point);
            w += rmm_input(j, i) * Fj;
          }
          partial_density += Fi * w;
        }
      }
      else {
        for (uint i = 0; i < group_m; i++) {
          float w = 0.0f;
          cfloat3 w3(0,0,0);
          cfloat3 ww1(0,0,0);
          cfloat3 ww2(0,0,0);

          float Fi = group.function_values(i, point);
          cfloat3 Fgi(group.gradient_values(i, point));
          cfloat3 Fhi1(group.hessian_values(2 * (i + 0) + 0, point));
          cfloat3 Fhi2(group.hessian_values(2 * (i + 0) + 1, point));

          for (uint j = 0; j <= i; j++) {
            float rmm = rmm_input(j,i);
            float Fj = group.function_values(j, point);
            w += Fj * rmm;

            cfloat3 Fgj(group.gradient_values(j, point));
            w3 += Fgj * rmm;

            cfloat3 Fhj1(group.hessian_values(2 * (j + 0) + 0, point));
            cfloat3 Fhj2(group.hessian_values(2 * (j + 0) + 1, point));
            ww1 += Fhj1 * rmm;
            ww2 += Fhj2 * rmm;
          }
          partial_density += Fi * w;

          dxyz += Fgi * w + w3 * Fi;
          dd1 += Fgi * w3 * 2 + Fhi1 * w + ww1 * Fi;

          cfloat3 FgXXY(Fgi.x(), Fgi.x(), Fgi.y());
          cfloat3 w3YZZ(w3.y(), w3.z(), w3.z());
          cfloat3 FgiYZZ(Fgi.y(), Fgi.z(), Fgi.z());
          cfloat3 w3XXY(w3.x(), w3.x(), w3.y());

          dd2 += FgXXY * w3YZZ + FgiYZZ * w3XXY + Fhi2 * w + ww2 * Fi;
        }
      }
      t_density.pause();

      t_forces.start();
      /** density derivatives **/
      if (compute_forces) {
        dd.resize(group.total_nucleii(), 1); dd.zero();
        for (uint i = 0, ii = 0; i < group.total_functions_simple(); i++) {
          uint nuc = group.func2local_nuc(ii);
          uint inc_i = group.small_function_type(i);
          cfloat3 this_dd(0,0,0);
          for (uint k = 0; k < inc_i; k++, ii++) {
            float w = 0.0f;
            for (uint j = 0; j < group_m; j++) {
              float Fj = group.function_values(j, point);
              w += rmm_input(j, ii) * Fj * (ii == j ? 2 : 1);
            }
            this_dd -= group.gradient_values(ii, point) * w;
          }
          dd(nuc) += this_dd;
        }
      }
      t_forces.pause();

      t_pot.start();

      t_density.start();
      /** energy / potential **/
      float exc = 0, corr = 0, y2a = 0;
      if (lda)
        cpu_pot(partial_density, exc, corr, y2a);
      else {
        cpu_potg(partial_density, dxyz, dd1, dd2, exc, corr, y2a);
      }

      t_pot.pause();

      if (compute_energy)
        total_energy += (partial_density * point_it->weight) * (exc + corr);
      t_density.pause();


      /** forces **/
      t_forces.start();
      if (compute_forces) {
        float factor = point_it->weight * y2a;
        for (uint i = 0; i < group.total_nucleii(); i++)
          forces(i) += dd(i) * factor;
      }
      t_forces.pause();

      /** RMM **/
      t_rmm.start();
      if (compute_rmm) {
        float factor = point_it->weight * y2a;
        HostMatrixFloat::blas_ssyr(HostMatrixFloat::LowerTriangle, factor, group.function_values, rmm_output, point);
      }
      t_rmm.pause();
    }

    t_forces.start();
    /* accumulate force results for this group */
    if (compute_forces) {
      FortranMatrix<double> fort_forces(fort_forces_ptr, fortran_vars.atoms, 3, FORTRAN_MAX_ATOMS); // TODO: mover esto a init.cpp
      for (uint i = 0; i < group.total_nucleii(); i++) {
        uint global_atom = group.local2global_nuc[i];
        cfloat3 this_force(forces(i));
        fort_forces(global_atom,0) += this_force.x();
        fort_forces(global_atom,1) += this_force.y();
        fort_forces(global_atom,2) += this_force.z();
      }
    }
    t_forces.pause();

    t_rmm.start();
    /* accumulate RMM results for this group */
    if (compute_rmm) {
      for (uint i = 0, ii = 0; i < group.total_functions_simple(); i++) {
        uint inc_i = group.small_function_type(i);
        for (uint k = 0; k < inc_i; k++, ii++) {
          uint big_i = group.local2global_func[i] + k;

          for (uint j = 0, jj = 0; j < group.total_functions_simple(); j++) {
            uint inc_j = group.small_function_type(j);
            for (uint l = 0; l < inc_j; l++, jj++) {
              uint big_j = group.local2global_func[j] + l;
              if (big_i > big_j) continue;

              uint big_index = (big_i * fortran_vars.m - (big_i * (big_i - 1)) / 2) + (big_j - big_i);
              fortran_vars.rmm_output(big_index) += rmm_output(ii, jj);
            }
          }
        }
      }
    }
    t_rmm.pause();

    #if CPU_RECOMPUTE
    /* clear functions */
    group.function_values.deallocate();
    group.gradient_values.deallocate();
    group.hessian_values.deallocate();
    #endif
  }

  /***** send results to fortran ****/
  if (compute_energy) *fort_energy_ptr = total_energy;

  t_total.stop();
  cout << "iteration: " << t_total << endl;
  cout << "rmm: " << t_rmm << " density: " << t_density << " pot: " << t_pot << " forces: " << t_forces << " resto: " << t_resto << " functions: " << t_functions << endl;
}

template void g2g_iteration<true, true, true>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<true, true, false>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<true, false, true>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<true, false, false>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<false, true, true>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<false, true, false>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<false, false, true>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
template void g2g_iteration<false, false, false>(bool compute_energy, double* fort_energy_ptr, double* fort_forces_ptr);
