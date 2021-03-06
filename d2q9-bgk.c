/*
** Code to implement a d2q9-bgk lattice boltzmann scheme.
** 'd2' inidates a 2-dimensional grid, and
** 'q9' indicates 9 velocities per grid cell.
** 'bgk' refers to the Bhatnagar-Gross-Krook collision step.
**
** The 'speeds' in each cell are numbered as follows:
**
** 6 2 5
**  \|/
** 3-0-1
**  /|\
** 7 4 8
**
** A 2D grid:
**
**           cols
**       --- --- ---
**      | D | E | F |
** rows  --- --- ---
**      | A | B | C |
**       --- --- ---
**
** 'unwrapped' in row major order to give a 1D array:
**
**  --- --- --- --- --- ---
** | A | B | C | D | E | F |
**  --- --- --- --- --- ---
**
** Grid indicies are:
**
**          ny
**          ^       cols(ii)
**          |  ----- ----- -----
**          | | ... | ... | etc |
**          |  ----- ----- -----
** rows(jj) | | 1,0 | 1,1 | 1,2 |
**          |  ----- ----- -----
**          | | 0,0 | 0,1 | 0,2 |
**          |  ----- ----- -----
**          ----------------------> nx
**
** Note the names of the input parameter and obstacle files
** are passed on the command line, e.g.:
**
**   ./d2q9-bgk input.params obstacles.dat
**
** Be sure to adjust the grid dimensions in the parameter file
** if you choose a different obstacle file.
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>

#include "mpi.h"

#define NSPEEDS 9
#define MASTER 0
#define FINALSTATEFILE "final_state.dat"
#define AVVELSFILE "av_vels.dat"

// #define DEBUG

/* struct to hold the parameter values */
typedef struct {
  int nx;           /* no. of cells in x-direction */
  int ny;           /* no. of cells in y-direction */
  int maxIters;     /* no. of iterations */
  int reynolds_dim; /* dimension for Reynolds number */
  float density;    /* density per link */
  float accel;      /* density redistribution */
  float omega;      /* relaxation parameter */
} t_param;

/* struct to hold the 'speed' values */
typedef struct {
  float speeds[NSPEEDS];
} t_speed;

/*
** function prototypes
*/

/* load params, allocate memory, load obstacles & initialise fluid particle
 * densities */
int initialise(const char* paramfile, const char* obstaclefile, t_param* params,
               t_speed** cells_ptr, t_speed** tmp_cells_ptr,
               int** obstacles_ptr, float** av_vels_ptr);

/*
** The main calculation methods.
** timestep calls, in order, the functions:
** accelerate_flow(), propagate(), rebound() & collision()
*/
int accelerate_flow(const t_param params, t_speed* cells, int* obstacles);
int propagate(int ii, int jj, const t_param params, t_speed* cells,
              t_speed* tmp_cells);
int rebound(int ii, int jj, const t_param params, t_speed* cells,
            t_speed* tmp_cells, int* obstacles);
int collision(int ii, int jj, const t_param params, t_speed* cells,
              t_speed* tmp_cells, int* obstacles);
int halo_exchange(t_speed* cells, float* sendbuf, float* recvbuf, int width,
                  int height, int domain_start, int domain_size, int rank,
                  int size);
int write_values(const t_param params, t_speed* cells, int* obstacles,
                 float* av_vels);

int sync_grid(t_speed* cells, int rank, int domain_start, int domain_size,
              int rows, int columns, int ranks);

/* finalise, including freeing up allocated memory */
int finalise(const t_param* params, t_speed** cells_ptr,
             t_speed** tmp_cells_ptr, int** obstacles_ptr, float** av_vels_ptr);

/* Sum all the densities in the grid.
** The total should remain constant from one timestep to the next. */
float total_density(const t_param params, t_speed* cells);

/* compute average velocity */
float av_velocity(const t_param params, t_speed* cells, int* obstacles,
                  int rank, int domain_start, int domain_size, int sync);

/* calculate Reynolds number */
float calc_reynolds(const t_param params, t_speed* cells, int* obstacles,
                    int height);

/* utility functions */
void die(const char* message, const int line, const char* file);
void usage(const char* exe);

/*
** main program:
** initialise, timestep loop, finalise
*/
int main(int argc, char* argv[]) {
  char* paramfile = NULL;    /* name of the input parameter file */
  char* obstaclefile = NULL; /* name of a the input obstacle file */
  t_param params;            /* struct to hold parameter values */
  t_speed* cells = NULL;     /* grid containing fluid densities */
  t_speed* tmp_cells = NULL; /* scratch space */
  int* obstacles = NULL;     /* grid indicating which cells are blocked */
  float* av_vels =
      NULL; /* a record of the av. velocity computed for each timestep */
  struct timeval timstr; /* structure to hold elapsed time */
  struct rusage ru;      /* structure to hold CPU time--system and user */
  double tic,
      toc; /* floating point numbers to calculate elapsed wallclock time */
  double usrtim; /* floating point number to record elapsed user CPU time */
  double systim; /* floating point number to record elapsed system CPU time */
  int rank;      /* 'rank' of process among it's cohort */
  int size;      /* size of cohort, i.e. num processes started */
  int domain_start; /* the starting y index of this process's domain */
  int domain_size;  /* the length of this process's domain */
  int flag;         /* for checking whether MPI_Init() has been called */
  enum bool { FALSE, TRUE }; /* enumerated type: false = 0, true = 1 */
  float* sendbuf;            /* buffer to hold values to send */
  float* recvbuf;            /* buffer to hold received values */

  /* parse the command line */
  if (argc != 3) {
    usage(argv[0]);
  } else {
    paramfile = argv[1];
    obstaclefile = argv[2];
  }

  MPI_Init(&argc, &argv);
  MPI_Initialized(&flag);
  if (flag != TRUE) {
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
  }

  /*
  ** determine the SIZE of the group of processes associated with
  ** the 'communicator'.  MPI_COMM_WORLD is the default communicator
  ** consisting of all the processes in the launched MPI 'job'
  */
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  /* determine the RANK of the current process [0:SIZE-1] */
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  /* initialise our data structures and load values from file */
  initialise(paramfile, obstaclefile, &params, &cells, &tmp_cells, &obstacles,
             &av_vels);

  /* calculate the size of the domain for this process */
  domain_start = 0;
  domain_size = 0;

  for (int i = 0; i < params.ny; ++i) {
    if (i % size == rank) {
      domain_size++;
    }
    if (i % size < rank) {
      domain_start++;
    }
  }

  sendbuf = malloc(sizeof(float) * NSPEEDS * params.nx);
  recvbuf = malloc(sizeof(float) * NSPEEDS * params.nx);

  /* iterate for maxIters timesteps */
  gettimeofday(&timstr, NULL);
  tic = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  for (int tt = 0; tt < params.maxIters; tt++) {
    if (rank == size - 1) {
      accelerate_flow(params, cells, obstacles);
    }

    halo_exchange(cells, sendbuf, recvbuf, params.nx, params.ny, domain_start,
                  domain_size, rank, size);

    for (int jj = domain_start; jj < domain_start + domain_size; ++jj) {
      for (int ii = 0; ii < params.nx; ++ii) {
        propagate(ii, jj, params, cells, tmp_cells);
        rebound(ii, jj, params, cells, tmp_cells, obstacles);
      }
    }
    for (int jj = domain_start; jj < domain_start + domain_size; ++jj) {
      for (int ii = 0; ii < params.nx; ++ii) {
        collision(ii, jj, params, cells, tmp_cells, obstacles);
      }
    }
    av_vels[tt] = av_velocity(params, cells, obstacles, rank, domain_start,
                              domain_size, 1);
#ifdef DEBUG
    printf("==timestep: %d==\n", tt);
    printf("av velocity: %.12E\n", av_vels[tt]);
    printf("tot density: %.12E\n", total_density(params, cells));
#endif
  }

  if (size != 1) {
    sync_grid(cells, rank, domain_start, domain_size, params.ny, params.nx,
              size);
  }

  gettimeofday(&timstr, NULL);
  toc = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
  getrusage(RUSAGE_SELF, &ru);
  timstr = ru.ru_utime;
  usrtim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
  timstr = ru.ru_stime;
  systim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  /* write final values and free memory */
  if (rank == MASTER) {
    printf("==done==\n");
    printf("Reynolds number:\t\t%.12E\n",
           calc_reynolds(params, cells, obstacles, params.ny));
    printf("Elapsed time:\t\t\t%.6lf (s)\n", toc - tic);
    printf("Elapsed user CPU time:\t\t%.6lf (s)\n", usrtim);
    printf("Elapsed system CPU time:\t%.6lf (s)\n", systim);
    write_values(params, cells, obstacles, av_vels);
  }
  finalise(&params, &cells, &tmp_cells, &obstacles, &av_vels);

  return EXIT_SUCCESS;
}

int accelerate_flow(const t_param params, t_speed* cells, int* obstacles) {
  /* compute weighting factors */
  float w1 = params.density * params.accel / 9.f;
  float w2 = params.density * params.accel / 36.f;

  /* modify the 2nd row of the grid */
  int jj = params.ny - 2;

  for (int ii = 0; ii < params.nx; ii++) {
    /* if the cell is not occupied and
    ** we don't send a negative density */
    if (!obstacles[ii + jj * params.nx] &&
        (cells[ii + jj * params.nx].speeds[3] - w1) > 0.f &&
        (cells[ii + jj * params.nx].speeds[6] - w2) > 0.f &&
        (cells[ii + jj * params.nx].speeds[7] - w2) > 0.f) {
      /* increase 'east-side' densities */
      cells[ii + jj * params.nx].speeds[1] += w1;
      cells[ii + jj * params.nx].speeds[5] += w2;
      cells[ii + jj * params.nx].speeds[8] += w2;
      /* decrease 'west-side' densities */
      cells[ii + jj * params.nx].speeds[3] -= w1;
      cells[ii + jj * params.nx].speeds[6] -= w2;
      cells[ii + jj * params.nx].speeds[7] -= w2;
    }
  }

  return EXIT_SUCCESS;
}

int propagate(int ii, int jj, const t_param params, t_speed* cells,
              t_speed* tmp_cells) {
  /* determine indices of axis-direction neighbours
  ** respecting periodic boundary conditions (wrap around) */
  int y_n = (jj + 1) % params.ny;
  int x_e = (ii + 1) % params.nx;
  int y_s = (jj == 0) ? (jj + params.ny - 1) : (jj - 1);
  int x_w = (ii == 0) ? (ii + params.nx - 1) : (ii - 1);
  /* propagate densities from neighbouring cells, following
  ** appropriate directions of travel and writing into
  ** scratch space grid */
  tmp_cells[ii + jj * params.nx].speeds[0] =
      cells[ii + jj * params.nx].speeds[0]; /* central cell, no movement */
  tmp_cells[ii + jj * params.nx].speeds[1] =
      cells[x_w + jj * params.nx].speeds[1]; /* east */
  tmp_cells[ii + jj * params.nx].speeds[2] =
      cells[ii + y_s * params.nx].speeds[2]; /* north */
  tmp_cells[ii + jj * params.nx].speeds[3] =
      cells[x_e + jj * params.nx].speeds[3]; /* west */
  tmp_cells[ii + jj * params.nx].speeds[4] =
      cells[ii + y_n * params.nx].speeds[4]; /* south */
  tmp_cells[ii + jj * params.nx].speeds[5] =
      cells[x_w + y_s * params.nx].speeds[5]; /* north-east */
  tmp_cells[ii + jj * params.nx].speeds[6] =
      cells[x_e + y_s * params.nx].speeds[6]; /* north-west */
  tmp_cells[ii + jj * params.nx].speeds[7] =
      cells[x_e + y_n * params.nx].speeds[7]; /* south-west */
  tmp_cells[ii + jj * params.nx].speeds[8] =
      cells[x_w + y_n * params.nx].speeds[8]; /* south-east */

  return EXIT_SUCCESS;
}

int rebound(int ii, int jj, const t_param params, t_speed* cells,
            t_speed* tmp_cells, int* obstacles) {
  /* if the cell contains an obstacle */
  if (obstacles[jj * params.nx + ii]) {
    /* called after propagate, so taking values from scratch space
    ** mirroring, and writing into main grid */
    cells[ii + jj * params.nx].speeds[1] =
        tmp_cells[ii + jj * params.nx].speeds[3];
    cells[ii + jj * params.nx].speeds[2] =
        tmp_cells[ii + jj * params.nx].speeds[4];
    cells[ii + jj * params.nx].speeds[3] =
        tmp_cells[ii + jj * params.nx].speeds[1];
    cells[ii + jj * params.nx].speeds[4] =
        tmp_cells[ii + jj * params.nx].speeds[2];
    cells[ii + jj * params.nx].speeds[5] =
        tmp_cells[ii + jj * params.nx].speeds[7];
    cells[ii + jj * params.nx].speeds[6] =
        tmp_cells[ii + jj * params.nx].speeds[8];
    cells[ii + jj * params.nx].speeds[7] =
        tmp_cells[ii + jj * params.nx].speeds[5];
    cells[ii + jj * params.nx].speeds[8] =
        tmp_cells[ii + jj * params.nx].speeds[6];
  }

  return EXIT_SUCCESS;
}

int collision(int ii, int jj, const t_param params, t_speed* cells,
              t_speed* tmp_cells, int* obstacles) {
  const float c_sq = 1.f / 3.f; /* square of speed of sound */
  const float w0 = 4.f / 9.f;   /* weighting factor */
  const float w1 = 1.f / 9.f;   /* weighting factor */
  const float w2 = 1.f / 36.f;  /* weighting factor */
  /* don't consider occupied cells */
  if (!obstacles[ii + jj * params.nx]) {
    /* compute local density total */
    float local_density = 0.f;

    for (int kk = 0; kk < NSPEEDS; kk++) {
      local_density += tmp_cells[ii + jj * params.nx].speeds[kk];
    }

    /* compute x velocity component */
    float u_x = (tmp_cells[ii + jj * params.nx].speeds[1] +
                 tmp_cells[ii + jj * params.nx].speeds[5] +
                 tmp_cells[ii + jj * params.nx].speeds[8] -
                 (tmp_cells[ii + jj * params.nx].speeds[3] +
                  tmp_cells[ii + jj * params.nx].speeds[6] +
                  tmp_cells[ii + jj * params.nx].speeds[7])) /
                local_density;
    /* compute y velocity component */
    float u_y = (tmp_cells[ii + jj * params.nx].speeds[2] +
                 tmp_cells[ii + jj * params.nx].speeds[5] +
                 tmp_cells[ii + jj * params.nx].speeds[6] -
                 (tmp_cells[ii + jj * params.nx].speeds[4] +
                  tmp_cells[ii + jj * params.nx].speeds[7] +
                  tmp_cells[ii + jj * params.nx].speeds[8])) /
                local_density;

    /* velocity squared */
    float u_sq = u_x * u_x + u_y * u_y;

    /* directional velocity components */
    float u[NSPEEDS];
    u[1] = u_x;        /* east */
    u[2] = u_y;        /* north */
    u[3] = -u_x;       /* west */
    u[4] = -u_y;       /* south */
    u[5] = u_x + u_y;  /* north-east */
    u[6] = -u_x + u_y; /* north-west */
    u[7] = -u_x - u_y; /* south-west */
    u[8] = u_x - u_y;  /* south-east */

    /* equilibrium densities */
    float d_equ[NSPEEDS];
    /* zero velocity density: weight w0 */
    d_equ[0] = w0 * local_density * (1.f - u_sq / (2.f * c_sq));
    /* axis speeds: weight w1 */
    d_equ[1] = w1 * local_density *
               (1.f + u[1] / c_sq + (u[1] * u[1]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    d_equ[2] = w1 * local_density *
               (1.f + u[2] / c_sq + (u[2] * u[2]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    d_equ[3] = w1 * local_density *
               (1.f + u[3] / c_sq + (u[3] * u[3]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    d_equ[4] = w1 * local_density *
               (1.f + u[4] / c_sq + (u[4] * u[4]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    /* diagonal speeds: weight w2 */
    d_equ[5] = w2 * local_density *
               (1.f + u[5] / c_sq + (u[5] * u[5]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    d_equ[6] = w2 * local_density *
               (1.f + u[6] / c_sq + (u[6] * u[6]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    d_equ[7] = w2 * local_density *
               (1.f + u[7] / c_sq + (u[7] * u[7]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));
    d_equ[8] = w2 * local_density *
               (1.f + u[8] / c_sq + (u[8] * u[8]) / (2.f * c_sq * c_sq) -
                u_sq / (2.f * c_sq));

    /* relaxation step */
    for (int kk = 0; kk < NSPEEDS; kk++) {
      cells[ii + jj * params.nx].speeds[kk] =
          tmp_cells[ii + jj * params.nx].speeds[kk] +
          params.omega *
              (d_equ[kk] - tmp_cells[ii + jj * params.nx].speeds[kk]);
    }
  }

  return EXIT_SUCCESS;
}
void SendRecv(t_speed* cells, float* sendbuf, float* recvbuf, int to, int from,
              int sendRow, int receiveRow, int id, int width) {
  MPI_Status status;
  for (int ii = 0; ii < width; ++ii) {
    for (int jj = 0; jj < NSPEEDS; ++jj) {
      sendbuf[ii * NSPEEDS + jj] = cells[ii + sendRow * width].speeds[jj];
    }
  }

  MPI_Sendrecv(sendbuf, width * NSPEEDS, MPI_FLOAT, to, id, recvbuf,
               width * NSPEEDS, MPI_FLOAT, from, id, MPI_COMM_WORLD, &status);

  for (int ii = 0; ii < width; ++ii) {
    for (int jj = 0; jj < NSPEEDS; ++jj) {
      cells[ii + receiveRow * width].speeds[jj] = recvbuf[ii * NSPEEDS + jj];
    }
  }
}

int halo_exchange(t_speed* cells, float* sendbuf, float* recvbuf, int width,
                  int height, int domain_start, int domain_size, int rank,
                  int size) {
  if (size != 1) {
    int to, from, sendRow, receiveRow;

    to = ((size + rank) - 1) % size;
    from = ((size + rank) + 1) % size;
    sendRow = domain_start;
    receiveRow = (domain_start + domain_size) % height;

    SendRecv(cells, sendbuf, recvbuf, to, from, sendRow, receiveRow, 0, width);

    to = ((size + rank) + 1) % size;
    from = ((rank + size) - 1) % size;
    sendRow = (domain_start + domain_size) - 1;
    receiveRow = ((domain_start + height) - 1) % height;
    SendRecv(cells, sendbuf, recvbuf, to, from, sendRow, receiveRow, 1, width);
  }
  return EXIT_SUCCESS;
}

float av_velocity(const t_param params, t_speed* cells, int* obstacles,
                  int rank, int domain_start, int domain_size, int sync) {
  int tot_cells = 0; /* no. of cells used in calculation */
  float tot_u;       /* accumulated magnitudes of velocity for each cell */

  /* initialise */
  tot_u = 0.f;

  /* loop over all non-blocked cells */
  for (int jj = domain_start; jj < domain_start + domain_size; jj++) {
    for (int ii = 0; ii < params.nx; ii++) {
      /* ignore occupied cells */
      if (!obstacles[ii + jj * params.nx]) {
        /* local density total */
        float local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++) {
          local_density += cells[ii + jj * params.nx].speeds[kk];
        }

        /* x-component of velocity */
        float u_x = (cells[ii + jj * params.nx].speeds[1] +
                     cells[ii + jj * params.nx].speeds[5] +
                     cells[ii + jj * params.nx].speeds[8] -
                     (cells[ii + jj * params.nx].speeds[3] +
                      cells[ii + jj * params.nx].speeds[6] +
                      cells[ii + jj * params.nx].speeds[7])) /
                    local_density;
        /* compute y velocity component */
        float u_y = (cells[ii + jj * params.nx].speeds[2] +
                     cells[ii + jj * params.nx].speeds[5] +
                     cells[ii + jj * params.nx].speeds[6] -
                     (cells[ii + jj * params.nx].speeds[4] +
                      cells[ii + jj * params.nx].speeds[7] +
                      cells[ii + jj * params.nx].speeds[8])) /
                    local_density;

        /* accumulate the norm of x- and y- velocity components */
        tot_u += sqrtf((u_x * u_x) + (u_y * u_y));
        /* increase counter of inspected cells */
        ++tot_cells;
      }
    }
  }

  if (sync == 1) {
    float sendbuf[2];
    float recvbuf[2];

    sendbuf[0] = tot_u;
    sendbuf[1] = (float)tot_cells;

    MPI_Reduce(&sendbuf, &recvbuf, 2, MPI_FLOAT, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0) {
      float result = recvbuf[0] / recvbuf[1];
      return result;
    } else {
      return tot_u / (float)tot_cells;
    }
  } else {
    return tot_u / (float)tot_cells;
  }
}

int sync_grid(t_speed* cells, int rank, int domain_start, int domain_size,
              int rows, int columns, int ranks) {
  if (rank != MASTER) {
    float* send = malloc(columns * domain_size * NSPEEDS * sizeof(float));

    for (int jj = 0; jj < domain_size; ++jj) {
      for (int ii = 0; ii < columns; ++ii) {
        for (int kk = 0; kk < NSPEEDS; ++kk) {
          send[kk + NSPEEDS * (ii + columns * jj)] =
              cells[ii + columns * (jj + domain_start)].speeds[kk];
        }
      }
    }

    MPI_Send(send, columns * domain_size * NSPEEDS, MPI_FLOAT, MASTER, 2,
             MPI_COMM_WORLD);
    free(send);
  } else {
    MPI_Status status;
    for (int i = 0; i < ranks; ++i) {
      if (i == MASTER) continue;
      int rank_start = 0;
      int rank_size = 0;

      for (int j = 0; j < rows; ++j) {
        if (j % ranks == i) {
          rank_size++;
        }
        if (j % ranks < i) {
          rank_start++;
        }
      }

      float* recv = malloc(columns * rank_size * NSPEEDS * sizeof(float));

      MPI_Recv(recv, columns * rank_size * NSPEEDS, MPI_FLOAT, i, 2,
               MPI_COMM_WORLD, &status);

      for (int jj = 0; jj < rank_size; ++jj) {
        for (int ii = 0; ii < columns; ++ii) {
          for (int kk = 0; kk < NSPEEDS; ++kk) {
            cells[ii + columns * (jj + rank_start)].speeds[kk] =
                recv[kk + NSPEEDS * (ii + columns * jj)];
          }
        }
      }
      free(recv);
    }
  }

  return EXIT_SUCCESS;
}

int initialise(const char* paramfile, const char* obstaclefile, t_param* params,
               t_speed** cells_ptr, t_speed** tmp_cells_ptr,
               int** obstacles_ptr, float** av_vels_ptr) {
  char message[1024]; /* message buffer */
  FILE* fp;           /* file pointer */
  int xx, yy;         /* generic array indices */
  int blocked;        /* indicates whether a cell is blocked by an obstacle */
  int retval;         /* to hold return value for checking */

  /* open the parameter file */
  fp = fopen(paramfile, "r");

  if (fp == NULL) {
    sprintf(message, "could not open input parameter file: %s", paramfile);
    die(message, __LINE__, __FILE__);
  }

  /* read in the parameter values */
  retval = fscanf(fp, "%d\n", &(params->nx));

  if (retval != 1) die("could not read param file: nx", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->ny));

  if (retval != 1) die("could not read param file: ny", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->maxIters));

  if (retval != 1)
    die("could not read param file: maxIters", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->reynolds_dim));

  if (retval != 1)
    die("could not read param file: reynolds_dim", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->density));

  if (retval != 1)
    die("could not read param file: density", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->accel));

  if (retval != 1) die("could not read param file: accel", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->omega));

  if (retval != 1) die("could not read param file: omega", __LINE__, __FILE__);

  /* and close up the file */
  fclose(fp);

  /*
  ** Allocate memory.
  **
  ** Remember C is pass-by-value, so we need to
  ** pass pointers into the initialise function.
  **
  ** NB we are allocating a 1D array, so that the
  ** memory will be contiguous.  We still want to
  ** index this memory as if it were a (row major
  ** ordered) 2D array, however.  We will perform
  ** some arithmetic using the row and column
  ** coordinates, inside the square brackets, when
  ** we want to access elements of this array.
  **
  ** Note also that we are using a structure to
  ** hold an array of 'speeds'.  We will allocate
  ** a 1D array of these structs.
  */

  /* main grid */
  *cells_ptr = (t_speed*)malloc(sizeof(t_speed) * (params->ny * params->nx));

  if (*cells_ptr == NULL)
    die("cannot allocate memory for cells", __LINE__, __FILE__);

  /* 'helper' grid, used as scratch space */
  *tmp_cells_ptr =
      (t_speed*)malloc(sizeof(t_speed) * (params->ny * params->nx));

  if (*tmp_cells_ptr == NULL)
    die("cannot allocate memory for tmp_cells", __LINE__, __FILE__);

  /* the map of obstacles */
  *obstacles_ptr = malloc(sizeof(int) * (params->ny * params->nx));

  if (*obstacles_ptr == NULL)
    die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  /* initialise densities */
  float w0 = params->density * 4.f / 9.f;
  float w1 = params->density / 9.f;
  float w2 = params->density / 36.f;

  for (int jj = 0; jj < params->ny; jj++) {
    for (int ii = 0; ii < params->nx; ii++) {
      /* centre */
      (*cells_ptr)[ii + jj * params->nx].speeds[0] = w0;
      /* axis directions */
      (*cells_ptr)[ii + jj * params->nx].speeds[1] = w1;
      (*cells_ptr)[ii + jj * params->nx].speeds[2] = w1;
      (*cells_ptr)[ii + jj * params->nx].speeds[3] = w1;
      (*cells_ptr)[ii + jj * params->nx].speeds[4] = w1;
      /* diagonals */
      (*cells_ptr)[ii + jj * params->nx].speeds[5] = w2;
      (*cells_ptr)[ii + jj * params->nx].speeds[6] = w2;
      (*cells_ptr)[ii + jj * params->nx].speeds[7] = w2;
      (*cells_ptr)[ii + jj * params->nx].speeds[8] = w2;
    }
  }

  /* first set all cells in obstacle array to zero */
  for (int jj = 0; jj < params->ny; jj++) {
    for (int ii = 0; ii < params->nx; ii++) {
      (*obstacles_ptr)[ii + jj * params->nx] = 0;
    }
  }

  /* open the obstacle data file */
  fp = fopen(obstaclefile, "r");

  if (fp == NULL) {
    sprintf(message, "could not open input obstacles file: %s", obstaclefile);
    die(message, __LINE__, __FILE__);
  }

  /* read-in the blocked cells list */
  while ((retval = fscanf(fp, "%d %d %d\n", &xx, &yy, &blocked)) != EOF) {
    /* some checks */
    if (retval != 3)
      die("expected 3 values per line in obstacle file", __LINE__, __FILE__);

    if (xx < 0 || xx > params->nx - 1)
      die("obstacle x-coord out of range", __LINE__, __FILE__);

    if (yy < 0 || yy > params->ny - 1)
      die("obstacle y-coord out of range", __LINE__, __FILE__);

    if (blocked != 1)
      die("obstacle blocked value should be 1", __LINE__, __FILE__);

    /* assign to array */
    (*obstacles_ptr)[xx + yy * params->nx] = blocked;
  }

  /* and close the file */
  fclose(fp);

  /*
  ** allocate space to hold a record of the avarage velocities computed
  ** at each timestep
  */
  *av_vels_ptr = (float*)malloc(sizeof(float) * params->maxIters);

  return EXIT_SUCCESS;
}

int finalise(const t_param* params, t_speed** cells_ptr,
             t_speed** tmp_cells_ptr, int** obstacles_ptr,
             float** av_vels_ptr) {
  /*
  ** free up allocated memory
  */
  free(*cells_ptr);
  *cells_ptr = NULL;

  free(*tmp_cells_ptr);
  *tmp_cells_ptr = NULL;

  free(*obstacles_ptr);
  *obstacles_ptr = NULL;

  free(*av_vels_ptr);
  *av_vels_ptr = NULL;

  /* finialise the MPI enviroment */
  MPI_Finalize();

  return EXIT_SUCCESS;
}

float calc_reynolds(const t_param params, t_speed* cells, int* obstacles,
                    int height) {
  const float viscosity = 1.f / 6.f * (2.f / params.omega - 1.f);

  return av_velocity(params, cells, obstacles, 0, 0, height, 0) *
         params.reynolds_dim / viscosity;
}

float total_density(const t_param params, t_speed* cells) {
  float total = 0.f; /* accumulator */

  for (int jj = 0; jj < params.ny; jj++) {
    for (int ii = 0; ii < params.nx; ii++) {
      for (int kk = 0; kk < NSPEEDS; kk++) {
        total += cells[ii + jj * params.nx].speeds[kk];
      }
    }
  }

  return total;
}

int write_values(const t_param params, t_speed* cells, int* obstacles,
                 float* av_vels) {
  FILE* fp;                     /* file pointer */
  const float c_sq = 1.f / 3.f; /* sq. of speed of sound */
  float local_density;          /* per grid cell sum of densities */
  float pressure;               /* fluid pressure in grid cell */
  float u_x;                    /* x-component of velocity in grid cell */
  float u_y;                    /* y-component of velocity in grid cell */
  float u; /* norm--root of summed squares--of u_x and u_y */

  fp = fopen(FINALSTATEFILE, "w");

  if (fp == NULL) {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int jj = 0; jj < params.ny; jj++) {
    for (int ii = 0; ii < params.nx; ii++) {
      /* an occupied cell */
      if (obstacles[ii + jj * params.nx]) {
        u_x = u_y = u = 0.f;
        pressure = params.density * c_sq;
      } /* no obstacle */ else {
        local_density = 0.f;

        for (int kk = 0; kk < NSPEEDS; kk++) {
          local_density += cells[ii + jj * params.nx].speeds[kk];
        }

        /* compute x velocity component */
        u_x = (cells[ii + jj * params.nx].speeds[1] +
               cells[ii + jj * params.nx].speeds[5] +
               cells[ii + jj * params.nx].speeds[8] -
               (cells[ii + jj * params.nx].speeds[3] +
                cells[ii + jj * params.nx].speeds[6] +
                cells[ii + jj * params.nx].speeds[7])) /
              local_density;
        /* compute y velocity component */
        u_y = (cells[ii + jj * params.nx].speeds[2] +
               cells[ii + jj * params.nx].speeds[5] +
               cells[ii + jj * params.nx].speeds[6] -
               (cells[ii + jj * params.nx].speeds[4] +
                cells[ii + jj * params.nx].speeds[7] +
                cells[ii + jj * params.nx].speeds[8])) /
              local_density;
        /* compute norm of velocity */
        u = sqrtf((u_x * u_x) + (u_y * u_y));
        /* compute pressure */
        pressure = local_density * c_sq;
      }

      /* write to file */
      fprintf(fp, "%d %d %.12E %.12E %.12E %.12E %d\n", ii, jj, u_x, u_y, u,
              pressure, obstacles[ii * params.nx + jj]);
    }
  }

  fclose(fp);

  fp = fopen(AVVELSFILE, "w");

  if (fp == NULL) {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int ii = 0; ii < params.maxIters; ii++) {
    fprintf(fp, "%d:\t%.12E\n", ii, av_vels[ii]);
  }

  fclose(fp);

  return EXIT_SUCCESS;
}

void die(const char* message, const int line, const char* file) {
  fprintf(stderr, "Error at line %d of file %s:\n", line, file);
  fprintf(stderr, "%s\n", message);
  fflush(stderr);
  exit(EXIT_FAILURE);
}

void usage(const char* exe) {
  fprintf(stderr, "Usage: %s <paramfile> <obstaclefile>\n", exe);
  exit(EXIT_FAILURE);
}
