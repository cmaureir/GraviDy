#include "dynamics_gpu.cuh"


__host__ void gpu_init_acc_jrk()
{
    int smem = BSIZE * 2* sizeof(double4);

    gpu_timer_start();
    k_init_acc_jrk <<< nblocks, nthreads, smem >>> (d_r, d_v, d_f, d_m, n,e2);
    cudaThreadSynchronize();
    float msec = gpu_timer_stop("k_init_acc_jrk");
    //float bytes = n * (32 + 32 + 4 + 48);
    get_kernel_error();

//    printf("Effective Bandwidth (GB/s): %f | time:%f  | bytes: %f \n", bytes/msec/1e6, msec, bytes);
//    printf("Effective Throughput (GFLOP/s) : %f | flops: %d | bajo: %f\n", (float)(60 * n * n)/(msec * 1e6), 60*n*n, msec*1e6);
//    printf("---\n");
}

__host__ double gpu_energy()
{

    CUDA_SAFE_CALL(cudaMemcpy(d_r,  h_r,  d4_size,cudaMemcpyHostToDevice));
    CUDA_SAFE_CALL(cudaMemcpy(d_v,  h_v,  d4_size,cudaMemcpyHostToDevice));

    gpu_timer_start();
    k_energy <<< nblocks, nthreads >>> (d_r, d_v, d_ekin, d_epot, d_m, n);
    cudaThreadSynchronize();
    float msec = gpu_timer_stop("k_energy");
    get_kernel_error();

    CUDA_SAFE_CALL(cudaMemcpy(h_ekin, d_ekin, d1_size,cudaMemcpyDeviceToHost));
    CUDA_SAFE_CALL(cudaMemcpy(h_epot, d_epot, d1_size,cudaMemcpyDeviceToHost));

    // Reduction on CPU
    ekin = 0.0;
    epot = 0.0;

    for (int i = 0; i < n; i++) {
        ekin += h_ekin[i];
        epot += h_epot[i];
    }

    return ekin + epot;
}

__host__ void gpu_predicted_pos_vel(float ITIME)
{

    gpu_timer_start();
    k_predicted_pos_vel<<< nblocks, nthreads >>> (d_r,
                                                  d_v,
                                                  d_f,
                                                  d_p,
                                                  d_t,
                                                  ITIME,
                                                  n);
    cudaThreadSynchronize();
    float msec = gpu_timer_stop("k_predicted_pos_vel");
    get_kernel_error();
}

__host__ void gpu_update(int total) {

    // Copying to the device the predicted r and v
    CUDA_SAFE_CALL(cudaMemcpy(d_p, h_p, sizeof(Predictor) * n,cudaMemcpyHostToDevice));

    // Fill the h_i Predictor array with the particles that we need
    // to move in this iteration
    for (int i = 0; i < total; i++) {
        int id = h_move[i];
        h_i[i] = h_p[id];
    }

    // Copy to the GPU (d_i) the preddictor host array (h_i)
    CUDA_SAFE_CALL(cudaMemcpy(d_i, h_i, sizeof(Predictor) * total, cudaMemcpyHostToDevice));


    // Blocks, threads and shared memory configuration
    dim3 nblocks(1 + (total-1)/BSIZE,NJBLOCK, 1);
    dim3 nthreads(BSIZE, 1, 1);
    size_t smem = BSIZE * sizeof(Predictor);

    // Kernel to update the forces for the particles in d_i
    gpu_timer_start();
    k_update <<< nblocks, nthreads, smem >>> (d_i, d_p, d_fout,d_m, n, total,e2);
    //float msec = gpu_timer_stop("k_update");
    float msec = gpu_timer_stop("");
   // printf("Effective Bandwidth (GB/s): %f | time:%f | total:%d | bytes: %d \n", (48 * (n + total + (total*16)))/msec/1e6, msec, total, (48 * (n + total + (total*16))));
   // printf("Effective Throughput (GFLOP/s) : %f\n", (60 * n * total)/(msec * 1e6));

    // Blocks, threads and shared memory configuration for the reduction.
    dim3 rgrid   (total,   1, 1);
    dim3 rthreads(NJBLOCK, 1, 1);
    size_t smem2 = sizeof(Forces) * NJBLOCK + 1;

    // Kernel to reduce que temp array with the forces
    gpu_timer_start();
    reduce <<< rgrid, rthreads, smem2 >>>(d_fout, d_fout_tmp, total);
    msec = gpu_timer_stop("reduce");
    get_kernel_error();

    // Copy from the GPU the new forces for the d_i particles.
    CUDA_SAFE_CALL(cudaMemcpy(h_fout_tmp, d_fout_tmp, sizeof(Forces) * total, cudaMemcpyDeviceToHost));

    // Update forces in the host
    for (int i = 0; i < total; i++) {
        int id = h_move[i];
        h_f[id] = h_fout_tmp[i];
    }

}
