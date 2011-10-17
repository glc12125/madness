#include <cuda.h>
//#include <world/cuda_streams.h>
#include <cuda_runtime.h>
#include <cublas_v2.h>
//#define NUM_STREAMS 16
//cudaStream_t streams[NUM_STREAMS];

extern "C" void * cublashandle_create(){
    cublasHandle_t * handle = new cublasHandle_t;
    cublasCreate(handle);
    void * h = (void *)handle;
    return h;
}

extern "C" void cublashandle_destroy(void * h){
    cublasHandle_t * handle = (cublasHandle_t*)h;
    cublasDestroy(*handle);
    delete handle;
}

extern "C" void ** streams_initialize(unsigned int streams){
    unsigned int i;
    void ** cast_streams = new void*[streams];
    for (i = 0; i < streams; i++){
        cudaStream_t * stream = new cudaStream_t;
        cudaStreamCreate(stream);
        cast_streams[i] = (void *)stream;
    }
    return cast_streams;
}


extern "C" void streams_destroy(void ** cast_streams, unsigned int streams){
    unsigned int i;
    for (i = 0; i < streams; i++){
        cudaStream_t * stream = (cudaStream_t *)cast_streams[i];
        cudaStreamDestroy(*stream);
        delete stream;
    }
}

extern "C" void streams_synchronize(void ** cast_streams, unsigned int streams){
    unsigned int i;
    for (i = 0; i < streams; i++){
        cudaStream_t * stream = (cudaStream_t *)cast_streams[i];
        cudaStreamSynchronize(*stream);
    }

}
