#ifndef __INIT_H__
#define __INIT_H__

#include "matrix.h"

/* definitions */
#define FORTRAN_MAX_ATOMS 170 
#define FORTRAN_NG 5100
#define FORTRAN_NL 20

namespace G2G {

  enum GridType {
    SMALL_GRID, MEDIUM_GRID, BIG_GRID
  };

  enum GridSize {
    SMALL_GRID_SIZE = 50, MEDIUM_GRID_SIZE = 116, BIG_GRID_SIZE = 194
  };

  struct FortranVars {
    uint atoms;
    bool normalize;
    float normalization_factor;
    uint s_funcs, p_funcs, d_funcs, spd_funcs, m;
    uint nco;
    uint iexch;
    bool do_forces;
    bool gga, lda;
    GridType grid_type;
    GridSize grid_size;
    FortranMatrix<double> atom_positions_pointer;
    HostMatrix<double3> atom_positions;
    HostMatrix<uint> atom_types;
    HostMatrix<uint> shells, shells1, shells2;
    HostMatrix<double> rm;
    HostMatrix<double> atom_atom_dists, nearest_neighbor_dists;
    FortranMatrix<uint> nucleii, contractions;
    FortranMatrix<double> a_values, c_values;
    FortranMatrix<double> rmm_input, rmm_input_ndens1, rmm_output;
    FortranMatrix<double> e, e1, e2, e3, wang, wang1, wang2, wang3;
  };

  extern FortranVars fortran_vars;

  extern uint max_function_exponent;
  extern double little_cube_size; // largo de una arista de los cubos chiquitos [Angstrom]
  extern uint min_points_per_cube;
  extern double becke_cutoff;
  extern bool assign_all_functions;
  extern double sphere_radius; // between 0 and 1!
  extern bool remove_zero_weights;
  extern bool energy_all_iterations;
}

#endif
