#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <ctime>
#include <cassert>
#include <vector_types.h>
#include <cuda_runtime_api.h>
#include <omp.h>

/*********************************
 *  Preprocessor directives
 *********************************/

/*
 * Debugging options
 */
//#define KERNEL_ERROR_DEBUG 1
#define INIT_PARTICLE 0  // Starting from 0 to include all the particles
#define RADIUS_MASS_PORCENTAGE 0.2
#define J 10 // Neighbour amount to calculate the center of density

/*
 * Softening parameter
 * (Please note that this parameter can be modified by the command line)
 */
#define E 1e-4
#define E2 1e-8

/*
 * ETA_N used to obtain the new time-step for a particle
 * using the equation (7) from Makino and Aarseth 1992.
 * (Please note that this parameter can be modified by the command line)
 *
 * ETA_S used to obtain the firsts time-steps for all the
 * particles of the system.
 */
#define ETA_S 0.01
#define ETA_N 0.01

/*
 * Time-step limits, used to restrict the values
 * using some minimum and maximum boundaries.
 *
 * The limits are powers of 2, because we use the block time-steps scheme.
 */
#define D_TIME_MIN (1.1920928955078125e-07) // 2e-23
#define D_TIME_MAX (0.125)                  // 2e-3

/*
 * Gravitational constant in N-body units
 */
#define G 1

/**********************
 * CUDA Configuration
 **********************/

#define BSIZE   32  // Block size on kernels calls
#define NJBLOCK 16  // Block size of the shared memory loading j-particles

// Macro from cutil.h to debug the CUDA calls
#define CUDA_SAFE_CALL_NO_SYNC( call) do {                          \
    cudaError err = call;                                             \
    if( cudaSuccess != err) {                                         \
        fprintf(stderr, "Cuda error in file '%s' in line %i : %s.\n", \
                __FILE__, __LINE__, cudaGetErrorString( err) );       \
        exit(EXIT_FAILURE);                                           \
    } } while (0)
#define CUDA_SAFE_CALL( call)     CUDA_SAFE_CALL_NO_SYNC(call);

/*******************************
       Structs definition
 *******************************/

typedef struct particle
{
    float m;
    double4 r;
    double4 v;
} particle;

typedef struct Predictor {
    double r[3];
    double v[3];
    float m;
} Predictor;

typedef struct Forces {
    double a[3];
    double a1[3];
} Forces;

typedef struct Gtime {
    // General integration times
    double integration_ini;
    double integration_end;

    // Prediction step times
    double prediction_ini;
    double prediction_end;

    // Force interaction general times
    double update_ini;
    double update_end;

    // Correction step times
    double correction_ini;
    double correction_end;

    // Gravitational interactions kernel times
    double grav_ini;
    double grav_end;

    // Force reductions kernel time
    double reduce_ini;
    double reduce_end;
} Gtime;

// Particle's vector to read the INPUT file
extern std::vector<particle> part;


/*
 * General variables of the program
 */
extern int n;                    // Number of particles on the system
extern int iterations;           // Number of iterations in the integration
extern std::string input_file;   // Input filename
extern std::string output_file;  // Output filename for general info.
extern FILE *out;                // Out file for debugging.
extern float total_mass;         // Total mass of the particles
                                 // (In N-body units will be 1)
extern Gtime gtime;              // Global structure to store time calculation
extern float gflops;             // GFLOPS count

extern float  itime;             // Integration time when the it will stop
extern double ekin, epot;        // Kinetic and Potential energy
extern double energy_ini;        // Initial energy of the system
extern double energy_end;        // Energy at an integration time t
extern double energy_tmp;        // Energy at an integration time t-1
extern float  e2, eta;           // Softening^2 and ETA parameters
                                 // (This parameters takes the previous
                                 // setted values E, and ETA_N or the
                                 // parameters give by the command line)

extern float t_rh;               // Half-mass relaxation time
extern float t_cr;               // Crossing time
extern size_t d1_size, d4_size;  // double and double4 size
extern size_t f1_size, i1_size;  // float and int size
extern size_t nthreads, nblocks; // Dynamical number of threads and blocks
                                 // for the GPU use.

/*********************************
 *  Host and Device pointers
 *********************************/

/*
 * Host pointers
 * (Particles attribute arrays)
 */
extern int       *h_move;         // Particles id to move in each iteration time
extern float     *h_m;            // Masses of the particles
extern double    *h_ekin, *h_epot;// Kinetic and Potential energy
extern double    *h_t, *h_dt;     // Time and time-step
extern double4   *h_r, *h_v;      // Position and Velocity
extern double4   *h_a2, *h_a3;    // 2nd and 3rd acceleration derivatives.
extern double4   *h_old_a;        // Previous step value of the Acceleration
extern double4   *h_old_a1;       // Previous step value of the Jerk
extern Predictor *h_p;
extern Predictor *h_i;
extern Forces    *h_f;            // Acceleration and its first derivative (Jerk)
extern Forces    *h_fout;
extern Forces    *h_fout_tmp;


/*
 * Device pointers
 * (Particles attribute arrays)
 */
extern int       *d_move;         // Particles id to move in each iteration time
extern float     *d_m;            // Masses of the particles
extern double    *d_ekin, *d_epot;// Kinetic and Potential energy
extern double    *d_t, *d_dt;     // Time and time-step
extern double4   *d_r, *d_v;      // Position and Velocity
extern Predictor *d_p;
extern Predictor *d_i;
extern Forces    *d_f;            // Acceleration and its first derivative (Jerk)
extern Forces    *d_fout;
extern Forces    *d_fout_tmp;

extern int print_log;
extern cudaEvent_t start, stop;

/************************************************
 * Special operators for the double4 data type
 ************************************************/
inline __host__ __device__ double4 operator+(const double4 &a, const double4 &b)
{
    double4 tmp = {a.x + b.x, a.y + b.y, a.z + b.z,  a.w + b.w};
    return tmp;
}

inline __host__ __device__ void operator+=(double4 &a, double4 &b)
{
    a.x += b.x;
    a.y += b.y;
    a.z += b.z;
    a.w += b.w;
}

inline __host__ __device__ Forces operator+(Forces &a, Forces &b)
{
    Forces tmp;
    tmp.a[0] = a.a[0] + b.a[0];
    tmp.a[1] = a.a[1] + b.a[1];
    tmp.a[2] = a.a[2] + b.a[2];

    tmp.a1[0] = a.a1[0] + b.a1[0];
    tmp.a1[1] = a.a1[1] + b.a1[1];
    tmp.a1[2] = a.a1[2] + b.a1[2];

    return tmp;
}

inline __host__ __device__ void operator+=(Forces &a, Forces &b)
{
    a.a[0] += b.a[0];
    a.a[1] += b.a[1];
    a.a[2] += b.a[2];

    a.a1[0] += b.a1[0];
    a.a1[1] += b.a1[1];
    a.a1[2] += b.a1[2];
}

