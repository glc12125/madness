/*
  This file is part of MADNESS.

  Copyright (C) 2007,2010 Oak Ridge National Laboratory

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

  For more information please contact:

  Robert J. Harrison
  Oak Ridge National Laboratory
  One Bethel Valley Road
  P.O. Box 2008, MS-6367

  email: harrisonrj@ornl.gov
  tel:   865-241-3937
  fax:   865-572-0680

  $Id$
*/
#ifndef MADNESS_MRA_OPERATOR_H__INCLUDED
#define MADNESS_MRA_OPERATOR_H__INCLUDED

/// \file mra/operator.h
/// \brief Implements most functionality of separated operators

/// \ingroup function

//extern "C" void daxpy_(const long*, const double*, const double*, const long*, double*, const long*);

#include <pthread.h>

#include <mra/mra.h>
#include <limits.h>
#include <mra/adquad.h>
#include <tensor/mtxmq.h>
#include <tensor/aligned.h>
#include <linalg/tensor_lapack.h>
#include <constants.h>

#include <mra/simplecache.h>
#include <mra/convolution1d.h>
#include <mra/displacements.h>

#include <world/GPU_streams.h>
#include <tensor/cu_mtxmq.h>

extern "C" void streams_synchronize(void **,unsigned int);
extern "C" void device_synchronize(void **,unsigned int);
extern "C" void** streams_initialize(unsigned int, void *);
namespace madness {

    template<typename Q>
    double xorhash(Tensor<Q> t){
        double hash = 1;
        for (int i = 0; i < t.size(); i++){
           hash += (300000*t.ptr()[i]);
        }
        return hash;
    }

    template<typename Q>
    double xorhash(const Q* t, int size){
        Q* temp = new Q[size];
        CPUtransfer_buffer(temp, const_cast<Q*>(t), size);
        double hash = 1;
        for (int i = 0; i < size; i++){
           hash += temp[i];
        }
        delete temp;
        return hash;
    }

    extern void truncate_periodic_expansion(Tensor<double>& c, Tensor<double>& e,
      double L, bool discardG0);

    extern void bsh_fit(double mu, double lo, double hi, double eps,
                            Tensor<double> *pcoeff, Tensor<double> *pexpnt, bool prnt=false);

    extern void bsh_fit_ndim(int ndim, double mu, double lo, double hi, double eps,
                                 Tensor<double> *pcoeff, Tensor<double> *pexpnt, bool prnt=false);

    template <typename Q, std::size_t NDIM>
    struct SeparatedConvolutionInternal {
        double norm;
        const ConvolutionData1D<Q>* ops[NDIM];
    };

    template <typename Q, std::size_t NDIM>
    struct SeparatedConvolutionData {
        std::vector< SeparatedConvolutionInternal<Q,NDIM> > muops;
        double norm;

        SeparatedConvolutionData(int rank) : muops(rank) {}
        SeparatedConvolutionData(const SeparatedConvolutionData<Q,NDIM>& q) {
            muops = q.muops;
            norm = q.norm;
        }
    };

    #if APPLY_GPU > 0
    template <typename Q, std::size_t NDIM>
    struct GPUApplyBuffer {
        Q* R;
        Q* T;
        Q* RU;
        Q* TU;
        Q* RVT;
        Q* TVT;
        bool * svd_done;

        //~GPUApplyBuffer(){
        //    delete svd_done;
        //}
    };
    #endif

    /// Convolutions in separated form (including Gaussian)
    template <typename Q, std::size_t NDIM>
    class SeparatedConvolution : public WorldObject< SeparatedConvolution<Q,NDIM> > {
    public:
        typedef SeparatedConvolutionData<Q,NDIM> SC;
        typedef Q opT;  ///< The apply function uses this to infer resultT=opT*inputT
        bool doleaves;  ///< If should be applied to leaf coefficients ... false by default
        bool isperiodicsum;///< If true the operator 1D kernels have been summed over lattice translations and may be non-zero at both ends of the unit cell
        struct Transformation {
            long r;             // Effective rank of transformation
            const Q* U;         // Ptr to matrix
            const Q* VT;
        };
    private:
        mutable std::vector< ConvolutionND<Q,NDIM> > ops;
        const BoundaryConditions<NDIM> bc;
        const int k;
        const int rank;
        const std::vector<long> vk;
        const std::vector<long> v2k;
        const std::vector<Slice> s0;

        mutable SimpleCache< SeparatedConvolutionData<Q,NDIM>, NDIM > data;
        mutable pthread_mutex_t apply_lock;
        #if APPLY_GPU > 0
        mutable SimpleCache< GPUApplyBuffer<Q,NDIM>, NDIM > GPUApplyCache;
        Q* apply_buffer;
        Q* GPUapply_buffer;
        long* r_buffer;
        long* r2_buffer;
        long* GPUr_buffer;
        long* GPUr2_buffer;
        mutable unsigned int apply_prev_offset;
        mutable unsigned int apply_curr_offset;
        static const unsigned int apply_buffer_maxsize = 1024*1024*30;
        static const unsigned int R_maxsize = 1024*1024*1;
        static const unsigned int T_maxsize = 1024*1024*1;
        #endif

        template <typename T, typename R>
        void apply_transformation1(Level n, long dimk,
                                  const Transformation trans[NDIM],
                                  const Tensor<T>& f,
                                  Tensor<R>& work1,
                                  Tensor<R>& work2,
                                  Tensor<Q>& work3,
                                  const Q mufac,
                                  Tensor<R>& result) const {

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            long size = 1;
            for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
            long dimi = size/dimk;

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
            Q* restrict w3=work3.ptr();

            const Q* U;

            ////U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
            U = trans[0].U;
            mTxmq(dimi, dimk, dimk, w1, f.ptr(), U);
            dimi = size/dimk;
            for (std::size_t d=1; d<NDIM; ++d) {
                U = trans[d].U;
                mTxmq(dimi, dimk, dimk, w2, w1, U);
                dimi = size/dimk;
                std::swap(w1,w2);
            }

            // If all blocks are full rank we can skip the transposes
            bool doit = false;
            for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

            if (doit) {
                for (std::size_t d=0; d<NDIM; ++d) {
                    if (trans[d].VT) {
                        dimi = size/dimk;
                        mTxmq(dimi, dimk, dimk, w2, w1, trans[d].VT);
                    }
                    else {
                        fast_transpose(dimk, dimi, w1, w2);
                    }
                    std::swap(w1,w2);
                }
            }
            // Assuming here that result is contiguous and aligned
            aligned_axpy(size, result.ptr(), w1, mufac);
            //    long one = 1;
            //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
        }

        template <typename T, typename R>
        void apply_transformation(Level n, long dimk,
                                  const Transformation trans[NDIM],
                                  const Tensor<T>& f,
                                  Tensor<R>& work1,
                                  Tensor<R>& work2,
                                  Tensor<Q>& work3,
                                  const Q mufac,
                                  Tensor<R>& result) const {

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            long size = 1;
            for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
            long dimi = size/dimk;

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
            Q* restrict w3=work3.ptr();

            const Q* U;

            U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
            mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
            size = trans[0].r * size / dimk;
            dimi = size/dimk;
            for (std::size_t d=1; d<NDIM; ++d) {
                U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
                size = trans[d].r * size / dimk;
                dimi = size/dimk;
                std::swap(w1,w2);
            }

            // If all blocks are full rank we can skip the transposes
            bool doit = false;
            for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

            if (doit) {
                for (std::size_t d=0; d<NDIM; ++d) {
                    if (trans[d].VT) {
                        dimi = size/trans[d].r;
                        mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
                        size = dimk*size/trans[d].r;
                    }
                    else {
                        fast_transpose(dimk, dimi, w1, w2);
                    }
                    std::swap(w1,w2);
                }
            }
            // Assuming here that result is contiguous and aligned
            aligned_axpy(size, result.ptr(), w1, mufac);
            //    long one = 1;
            //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
        }

        /// Apply one of the separated terms, accumulating into the result
        template <typename T>
        void muopxv_fast(Level n,
                         const ConvolutionData1D<Q>* const ops[NDIM],
                         const Tensor<T>& f, const Tensor<T>& f0,
                         Tensor<TENSOR_RESULT_TYPE(T,Q)>& result,
                         Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0,
                         double tol,
                         const Q mufac,
                         Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1,
                         Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2,
                         Tensor<Q>& work5) const {

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            Transformation trans[NDIM];

            double Rnorm = 1.0;
            for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
            if (Rnorm == 0.0) return;

            tol = tol/(Rnorm*NDIM);  // Errors are relative within here

            // Determine rank of SVD to use or if to use the full matrix
            const long twok = 2*k;
            long break_even;
            if (NDIM==1) break_even = long(0.5*twok);
            else if (NDIM==2) break_even = long(0.6*twok);
            else if (NDIM==3) break_even=long(0.65*twok);
            else break_even=long(0.7*twok);
            for (std::size_t d=0; d<NDIM; ++d) {
                long r;
                for (r=0; r<twok; ++r) {
                    if (ops[d]->Rs[r] < tol) break;
                }
                if (r >= break_even) {
                    trans[d].r = twok;
                    trans[d].U = ops[d]->R.ptr();
                    trans[d].VT = 0;
                }
                else {
                    r += (r&1L);
                    trans[d].r = std::max(2L,r);
                    trans[d].U = ops[d]->RU.ptr();
                    trans[d].VT = ops[d]->RVT.ptr();
                }
            }
            apply_transformation/*1*/(n, twok, trans, f, work1, work2, work5, mufac, result);

            if (n > 0) {
                if (NDIM==1) break_even = long(0.5*k);
                else if (NDIM==2) break_even = long(0.6*k);
                else if (NDIM==3) break_even=long(0.65*k);
                else break_even=long(0.7*k);
                for (std::size_t d=0; d<NDIM; ++d) {
                    long r;
                    for (r=0; r<k; ++r) {
                        if (ops[d]->Ts[r] < tol) break;
                    }
                    if (r >= break_even) {
                        trans[d].r = k;
                        trans[d].U = ops[d]->T.ptr();
                        trans[d].VT = 0;
                    }
                    else {
                        r += (r&1L);
                        trans[d].r = std::max(2L,r);
                        trans[d].U = ops[d]->TU.ptr();
                        trans[d].VT = ops[d]->TVT.ptr();
                    }
                }
                apply_transformation/*1*/(n, k, trans, f0, work1, work2, work5, -mufac, result0);
            }
        }


        /// Computes the Frobenius norm of one of the separated terms ... WITHOUT FACTOR INCLUDED
        double munorm2(Level n, const ConvolutionData1D<Q>* ops[]) const {
            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            double prodR=1.0, prodT=1.0;
            for (std::size_t d=0; d<NDIM; ++d) {
                prodR *= ops[d]->Rnormf;
                prodT *= ops[d]->Tnormf;

            }
            if (n) prodR = sqrt(std::max(prodR*prodR - prodT*prodT,0.0));

            if (prodR < 1e-8*prodT) {
                double prod=1.0, sum=0.0;
                for (std::size_t d=0; d<NDIM; ++d) {
                    double a = ops[d]->NSnormf;
                    double b = ops[d]->Tnormf;
                    double aa = std::min(a,b);
                    double bb = std::max(a,b);
                    prod *= bb;
                    if (bb > 0.0) sum +=(aa/bb);
                }
                if (n) prod *= sum;
                prodR = prod;
            }

            return prodR;
        }


        const SeparatedConvolutionInternal<Q,NDIM> getmuop(int mu, Level n, const Key<NDIM>& disp) const {
            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            SeparatedConvolutionInternal<Q,NDIM> op;
            for (std::size_t d=0; d<NDIM; ++d) {
                op.ops[d] = ops[mu].getop(d)->nonstandard(n, disp.translation()[d]);
            }
            op.norm = munorm2(n, op.ops)*std::abs(ops[mu].getfac());

//             double newnorm = munorm2(n, op.ops);
//             // This rescaling empirically based upon BSH separated expansion
//             // ... needs more testing.  OK also for TDSE.
//             // All is good except for some 000 blocks which are up to sqrt(k^d) off.
//             for (int d=0; d<NDIM; ++d)  {
//                 if (disp[d] == 0) newnorm *= 0.5;
//                 else if (std::abs(disp[d]) == 1) newnorm *= 0.8;
//             }
//            double oldnorm = munorm(n, op.ops);
//             if (oldnorm > 1e-13 && (newnorm < 0.5*oldnorm || newnorm > 2.0*oldnorm) )
//                 print("munorm", n, disp, mu, newnorm, oldnorm, newnorm/oldnorm);

            return op;
        }


        /// Returns pointer to cached operator
        const SeparatedConvolutionData<Q,NDIM>* getop(Level n, const Key<NDIM>& d) const {
            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            const SeparatedConvolutionData<Q,NDIM>* p = data.getptr(n,d);
            if (p){
              //print ("CACHED"); 
              return p;
            }
            else{
              //print("NOT");
            }

            SeparatedConvolutionData<Q,NDIM> op(rank);
            for (int mu=0; mu<rank; ++mu) {
                op.muops[mu] = getmuop(mu, n, d);
            }

            double norm = 0.0;
            for (int mu=0; mu<rank; ++mu) {
                const double munorm = op.muops[mu].norm;
                norm += munorm*munorm;
            }
	    //print("getop", n, d, norm);
            op.norm = sqrt(norm);
            data.set(n, d, op);
            return data.getptr(n,d);
        }

        void check_cubic() {
            // !!! NB ... cell volume obtained from global defaults
            const Tensor<double>& cell_width = FunctionDefaults<NDIM>::get_cell_width();
            // Check that the cell is cubic since currently is assumed
            for (std::size_t d=1; d<NDIM; ++d) {
                MADNESS_ASSERT(fabs(cell_width(d)-cell_width(0L)) < 1e-14*cell_width(0L));
            }
        }

    public:
        int getHashVal(){
            return 1000*rank+k;
        }
        // Default constructor for invoking compute member functions on aggregate arguments
        // BAD Constructor: it does NOT call process_pending()
        SeparatedConvolution(World * w) :WorldObject<SeparatedConvolution<Q,NDIM> >(*w), k(0), rank(0) {
        }

        // For separated convolutions with same operator in each direction (isotropic)
        SeparatedConvolution(World& world,
                             std::vector< std::shared_ptr< Convolution1D<Q> > >& argops,
                             const BoundaryConditions<NDIM>& bc = FunctionDefaults<NDIM>::get_bc(),
                             long k = FunctionDefaults<NDIM>::get_k(),
                             bool doleaves = false)
                : WorldObject< SeparatedConvolution<Q,NDIM> >(world)
                , doleaves(doleaves)
                , isperiodicsum(bc(0,0)==BC_PERIODIC)
                , bc(bc)
                , k(k)
                , rank(argops.size())
                , vk(NDIM,k)
                , v2k(NDIM,2*k)
                , s0(std::max<std::size_t>(2,NDIM),Slice(0,k-1))
        {
            print("SeparatedConvolution object with rank = ",rank," k = ",k," NDIM = ",NDIM," constructed.");
            // Presently we must have periodic or non-periodic in all dimensions.
            for (std::size_t d=1; d<NDIM; ++d) {
                MADNESS_ASSERT(bc(d,0)==bc(0,0));
            }
            check_cubic();

            for (unsigned int mu=0; mu < argops.size(); ++mu) {
              this->ops.push_back(ConvolutionND<Q,NDIM>(argops[mu]));
            }

            #if APPLY_GPU > 0
            pthread_mutex_init(&apply_lock, NULL);
            alloc_host(&apply_buffer,MAX_AGG*rank*NDIM*3*(2*k*2*k + k*k));
            GPUapply_buffer = GPUtransfer_buffer(apply_buffer, apply_buffer_maxsize , false);
            apply_prev_offset = 0;
            apply_curr_offset = 0;
            alloc_host(&r_buffer,MAX_AGG*rank*NDIM);
            GPUr_buffer = GPUtransfer_buffer(r_buffer, R_maxsize, false);
            alloc_host(&r2_buffer,MAX_AGG*rank*NDIM);
            GPUr2_buffer = GPUtransfer_buffer(r2_buffer, R_maxsize, false);
            #endif

            this->process_pending();
        }

        //copy constructor
        /*SeparatedConvolution(const SeparatedConvolution& sc)
                : WorldObject< SeparatedConvolution<Q,NDIM> >(sc.world)
                , doleaves(sc.doleaves)
                , isperiodicsum(sc.isperiodicsum)
                , ops(sc.ops)
                , bc(sc.bc)
                , k(sc.k)
                , rank(sc.rank)
                , vk(sc.vk)
                , v2k(sc.v2k)
                , s0(sc.s0)
        {
            this->process_pending();
        }*/
        // For general convolutions
        SeparatedConvolution(World& world,
                             std::vector< ConvolutionND<Q,NDIM> >& argops,
                             const BoundaryConditions<NDIM>& bc = FunctionDefaults<NDIM>::get_bc(),
                             long k = FunctionDefaults<NDIM>::get_k(),
                             bool doleaves = false)
                : WorldObject< SeparatedConvolution<Q,NDIM> >(world)
                , doleaves(doleaves)
                , isperiodicsum(bc(0,0)==BC_PERIODIC)
                , ops(argops)
                , bc(bc)
                , k(k)
                , rank(argops.size())
                , vk(NDIM,k)
                , v2k(NDIM,2*k)
                , s0(std::max<std::size_t>(2,NDIM),Slice(0,k-1))
        {
            print("SeparatedConvolution object with rank = ",rank," k = ",k," NDIM = ",NDIM," constructed.");
            // Presently we must have periodic or non-periodic in all dimensions.
            for (std::size_t d=1; d<NDIM; ++d) {
                MADNESS_ASSERT(bc(d,0)==bc(0,0));
            }
            this->process_pending();

            #if APPLY_GPU > 0
            pthread_mutex_init(&apply_lock, NULL);
            alloc_host(&apply_buffer,MAX_AGG*rank*NDIM*3*(2*k*2*k + k*k));
            GPUapply_buffer = GPUtransfer_buffer(apply_buffer, apply_buffer_maxsize, false);
            apply_prev_offset = 0;
            apply_curr_offset = 0;
            alloc_host(&r_buffer,MAX_AGG*rank*NDIM);
            GPUr_buffer = GPUtransfer_buffer(r_buffer, R_maxsize, false);
            alloc_host(&r2_buffer,MAX_AGG*rank*NDIM);
            GPUr2_buffer = GPUtransfer_buffer(r2_buffer, R_maxsize, false);
            #endif
        }

        /// Constructor for Gaussian Convolutions (mostly for backward compatability)
        SeparatedConvolution(World& world,
                             const Tensor<Q>& coeff, const Tensor<double>& expnt,
                             const BoundaryConditions<NDIM>& bc = FunctionDefaults<NDIM>::get_bc(),
                             int k=FunctionDefaults<NDIM>::get_k(),
                             bool doleaves = false)
                : WorldObject< SeparatedConvolution<Q,NDIM> >(world)
                , doleaves(doleaves)
                , isperiodicsum(bc(0,0)==BC_PERIODIC)
                , ops(coeff.dim(0))
                , bc(bc)
                , k(k)
                , rank(coeff.dim(0))
                , vk(NDIM,k)
                , v2k(NDIM,2*k)
                , s0(std::max<std::size_t>(2,NDIM),Slice(0,k-1))
        {
            print("SeparatedConvolution object with rank = ",rank," k = ",k," NDIM = ",NDIM," constructed.");
            // Presently we must have periodic or non-periodic in all dimensions.
            for (std::size_t d=1; d<NDIM; ++d) {
                MADNESS_ASSERT(bc(d,0)==bc(0,0));
            }

            const Tensor<double>& width = FunctionDefaults<NDIM>::get_cell_width();
            const double pi = constants::pi;

            for (int mu=0; mu<rank; ++mu) {
                Q c = std::pow(sqrt(expnt(mu)/pi),static_cast<int>(NDIM)); // Normalization coeff

                // We cache the normalized operator so the factor is the value we must multiply
                // by to recover the coeff we want.
                ops[mu].setfac(coeff(mu)/c);

                for (std::size_t d=0; d<NDIM; ++d) {
                  ops[mu].setop(d,GaussianConvolution1DCache<Q>::get(k, expnt(mu)*width[d]*width[d], 0, isperiodicsum));
                }
            }

            #if APPLY_GPU > 0
            pthread_mutex_init(&apply_lock, NULL);
            alloc_host(&apply_buffer,MAX_AGG*rank*NDIM*3*(2*k*2*k + k*k));
            GPUapply_buffer = GPUtransfer_buffer(apply_buffer, apply_buffer_maxsize, false);
            apply_prev_offset = 0;
            apply_curr_offset = 0;
            alloc_host(&r_buffer,MAX_AGG*rank*NDIM);
            GPUr_buffer = GPUtransfer_buffer(r_buffer, R_maxsize, false);
            alloc_host(&r2_buffer,MAX_AGG*rank*NDIM);
            GPUr2_buffer = GPUtransfer_buffer(r2_buffer, R_maxsize, false);
            #endif
        }

        /// WSTHORNTON Constructor for Gaussian Convolutions (mostly for backward compatability)
        SeparatedConvolution(World& world,
                             Vector<double,NDIM> args,
                             const Tensor<Q>& coeff, const Tensor<double>& expnt,
                             const BoundaryConditions<NDIM>& bc = FunctionDefaults<NDIM>::get_bc(),
                             int k=FunctionDefaults<NDIM>::get_k(),
                             bool doleaves=false)
                : WorldObject< SeparatedConvolution<Q,NDIM> >(world)
                , doleaves(doleaves)
                , isperiodicsum(bc(0,0)==BC_PERIODIC)
                , ops(coeff.dim(0))
                , bc(bc)
                , k(k)
                , rank(coeff.dim(0))
                , vk(NDIM,k)
                , v2k(NDIM,2*k)
                , s0(std::max<std::size_t>(2,NDIM),Slice(0,k-1))
        {
            print("SeparatedConvolution object with rank = ",rank," k = ",k," NDIM = ",NDIM," constructed.");
            // Presently we must have periodic or non-periodic in all dimensions.
            for (std::size_t d=1; d<NDIM; ++d) {
                MADNESS_ASSERT(bc(d,0)==bc(0,0));
            }

            const Tensor<double>& width = FunctionDefaults<NDIM>::get_cell_width();

            for (int mu=0; mu<rank; ++mu) {
                for (std::size_t d=0; d<NDIM; ++d) {
                  Q c = pow(coeff[mu],1.0/NDIM);
                  std::shared_ptr<GaussianConvolution1D<Q> >
                      gcptr(new GaussianConvolution1D<Q>(k, c*width[d], expnt(mu)*width[d]*width[d],
                              0, isperiodicsum, args[d]));
                  ops[mu].setop(d,gcptr);
                }
            }

            #if APPLY_GPU > 0
            pthread_mutex_init(&apply_lock, NULL);
            alloc_host(&apply_buffer,MAX_AGG*rank*NDIM*3*(2*k*2*k + k*k));
            GPUapply_buffer = GPUtransfer_buffer(apply_buffer, apply_buffer_maxsize, false);
            apply_prev_offset = 0;
            apply_curr_offset = 0;
            alloc_host(&r_buffer,MAX_AGG*rank*NDIM);
            GPUr_buffer = GPUtransfer_buffer(r_buffer, R_maxsize, false);
            alloc_host(&r2_buffer,MAX_AGG*rank*NDIM);
            GPUr2_buffer = GPUtransfer_buffer(r2_buffer, R_maxsize, false);
            #endif
        }

        virtual ~SeparatedConvolution() { 
            print("SeparatedConvolution object with rank = ",rank," k = ",k," NDIM = ",NDIM," destructed.");
            #if APPLY_GPU > 0
            dealloc_host(apply_buffer); GPUdelete_buffer(GPUapply_buffer); 
            dealloc_host(r_buffer); GPUdelete_buffer(GPUr_buffer);
            dealloc_host(r2_buffer); GPUdelete_buffer(GPUr2_buffer); 
            #endif 
            }

        const BoundaryConditions<NDIM>& get_bc() const {return bc;}

        const std::vector< Key<NDIM> > get_disp(Level n) const {
            return Displacements<NDIM>().get_disp(n, isperiodicsum);
        }

        double norm(Level n, const Key<NDIM>& d) const {
            return getop(n, d)->norm;
        }

        template <typename T>
        Function<TENSOR_RESULT_TYPE(T,Q),NDIM> operator()(const Function<T,NDIM>& f) const {
            return madness::apply(*this, f);
        }


        template <typename T>
        Tensor<TENSOR_RESULT_TYPE(T,Q)> apply(const Key<NDIM>& source,
                                              const Key<NDIM>& shift,
                                              const Tensor<T>& coeff,
                                              double tol) const {
            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            typedef TENSOR_RESULT_TYPE(T,Q) resultT;
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            //STARTt_TIMER;
            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k), r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);

            const Tensor<T> f0 = copy(coeff(s0));
            for (int mu=0; mu<rank; ++mu) { //parallel loop, but reductions are a problem
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu]; //same for all aplies of same instance
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();  //same for all aplies of same instance
                    muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                                work1, work2, work5);
                }
                else{
                    //double Rnormf = 1.0;
                    //for (int d = 0; d < NDIM; d++){
                    //    Rnormf *= muop.ops[d]->Rnormf;
                    //}
                    //if (Rnormf == 0){
                    //for (int d = 0; d < NDIM; d++){
                    //  MADNESS_ASSERT(muop.ops[d]->R.ptr() == 0);
                    //}
                    //}
                }
            }
            r(s0).gaxpy(1.0,r0,1.0);
            //ENDt_TIMER("cpu-agg");
            return r;
        }

        typedef Key<NDIM> keyT;


        template <typename T>
        std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> 
        apply_computept(std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > >& t1) const{
            
          typedef TENSOR_RESULT_TYPE(T,Q) resultT;
          typedef resultT R;
          typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

          /*const*/ keyT/*&*/ argskey = std::tr1::get<0>(t1);
          /*const*/ keyT/*&*/ argsd = std::tr1::get<1>(t1);
          keyT/*&*/ argsdest = std::tr1::get<2>(t1);
          double/*&*/ argstol = std::tr1::get<3>(t1);
          double/*&*/ argsfac = std::tr1::get<4>(t1);
          double/*&*/ argscnorm = std::tr1::get<5>(t1);
          /*const*/ Tensor<R>/*&*/ coeff = std::tr1::get<6>(t1);
          dcT/*&*/ coeffs = std::tr1::get<7>(t1);

          /*const*/ Key<NDIM>/*&*/ source = argskey;
          /*const*/ Key<NDIM>/*&*/ shift = argsd;
          double tol = argstol/argsfac/argscnorm;

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            //print("inlined_apply \n");
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k);
            Tensor<resultT> r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            //std::vector<Slice> s1(std::max<std::size_t>(2,NDIM),Slice(0,k-1))

            const Tensor<T> f0 = copy(coeff(s0));
            
            Level n = source.level();
            const Tensor<T>& f = *input;
            Transformation trans[NDIM];
	    const long twok = 2*k;
	    long break_even;
	    
            if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
            long break_even2;
            if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0) continue;

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[d].r = twok;
			    trans[d].U = ops[d]->R.ptr();
			    trans[d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[d].r = std::max(2L,r1);
			    trans[d].U = ops[d]->RU.ptr();
			    trans[d].VT = ops[d]->RVT.ptr();
			}
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		   
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }

		    // If all blocks are full rank we can skip the transposes
		    bool doit = false;
		    for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
                                ////GPU
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
                                ////GPU
				fast_transpose(dimk, dimi, w1, w2);
			    }
                            ////GPU
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);

		    if (n > 0) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans[d].r = k;
				trans[d].U = ops[d]->T.ptr();
				trans[d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans[d].r = std::max(2L,r1);
				trans[d].U = ops[d]->TU.ptr();
				trans[d].VT = ops[d]->TVT.ptr();
			    }
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                        ////GPU
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                            ////GPU
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
                            ////GPU
		            std::swap(w1,w2);
			}

			// If all blocks are full rank we can skip the transposes
			bool doit = false;
			for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
                                    ////GPU
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				    size = dimk*size/trans[d].r;
				}
				else {
                                    ////GPU
			            fast_transpose(dimk, dimi, w1, w2);
				}
                                ////GPU
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
                         ////GPU
			 aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    }
                }
            }
            Tensor<R> * r1 = new Tensor<R>(r); 
            Tensor<R> * r01 = new Tensor<R>(r0); 
            std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, coeffs, argsdest, argstol, argsfac);
            return t2;  
        }

        template <typename T>
        std::tr1::tuple<bool*, Transformation**, Transformation**,
                        bool*, bool*, Q*, Level, keyT, double, double,
                        WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                        Tensor<T>, Tensor<T> > 
        apply_computepreprocess(std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > >& t1) const{
            
          typedef TENSOR_RESULT_TYPE(T,Q) resultT;
          typedef resultT R;
          typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

          /*const*/ keyT/*&*/ argskey = std::tr1::get<0>(t1);
          /*const*/ keyT/*&*/ argsd = std::tr1::get<1>(t1);
          keyT/*&*/ argsdest = std::tr1::get<2>(t1);
          double/*&*/ argstol = std::tr1::get<3>(t1);
          double/*&*/ argsfac = std::tr1::get<4>(t1);
          double/*&*/ argscnorm = std::tr1::get<5>(t1);
          /*const*/ Tensor<R>/*&*/ coeff = std::tr1::get<6>(t1);
          dcT/*&*/ coeffs = std::tr1::get<7>(t1);

          /*const*/ Key<NDIM>/*&*/ source = argskey;
          /*const*/ Key<NDIM>/*&*/ shift = argsd;
          double tol = argstol/argsfac/argscnorm;

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            //print("inlined_apply \n");
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getopGPU(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k);
            Tensor<resultT> r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            //std::vector<Slice> s1(std::max<std::size_t>(2,NDIM),Slice(0,k-1))

            const Tensor<T> f0 = copy(coeff(s0));
            
            Level n = source.level();
            const Tensor<T>& f = *input;
            Transformation ** trans = new Transformation*[rank];
            Transformation ** trans2 = new Transformation*[rank];
            for (int mu = 0; mu < rank; mu++){
                trans[mu] = new Transformation[NDIM];
                trans2[mu] = new Transformation[NDIM];
            }

            bool* condition = new bool[rank];

            bool* doit2;
            bool* doit1;
            doit2 = new bool[rank];
            doit1 = new bool[rank];
            for (int mu = 0; mu < rank; mu++){
                doit2[mu] = false;
                doit1[mu] =  false;
            }
            
            Q* mufacs = new Q[rank];
 
	    const long twok = 2*k;
	    long break_even;
	    
            if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
            long break_even2;
            if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    condition[mu] = true;
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    mufacs[mu] = mufac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0){
                        condition[mu] = false;
                        continue;
                    }

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[mu][d].r = twok;
			    trans[mu][d].U = ops[d]->R.ptr();
			    trans[mu][d].U = ops[d]->GPUR;
			    trans[mu][d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[mu][d].r = std::max(2L,r1);
			    trans[mu][d].U = ops[d]->RU.ptr();
			    trans[mu][d].U = ops[d]->GPURU;
			    trans[mu][d].VT = ops[d]->RVT.ptr();
			    trans[mu][d].VT = ops[d]->GPURVT;
			}
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		   
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

                    /*
		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }
                    */

		    // If all blocks are full rank we can skip the transposes
		    for (std::size_t d=0; d<NDIM; ++d) doit2[mu] = doit2[mu] || trans[mu][d].VT;

                    /*
		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
                                ////GPU
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
                                ////GPU
				fast_transpose(dimk, dimi, w1, w2);
			    }
                            ////GPU
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    */

		    if (n > 0) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans2[mu][d].r = k;
				trans2[mu][d].U = ops[d]->T.ptr();
				trans2[mu][d].U = ops[d]->GPUT;
				trans2[mu][d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans2[mu][d].r = std::max(2L,r1);
				trans2[mu][d].U = ops[d]->TU.ptr();
				trans2[mu][d].U = ops[d]->GPUTU;
				trans2[mu][d].VT = ops[d]->TVT.ptr();
				trans2[mu][d].VT = ops[d]->GPUTU;
			    }
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

                        /*
			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                        ////GPU
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                            ////GPU
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
                            ////GPU
		            std::swap(w1,w2);
			}
                        */

			// If all blocks are full rank we can skip the transposes
			for (std::size_t d=0; d<NDIM; ++d) doit1[mu] = doit1[mu] || trans2[mu][d].VT;

                        /*
			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
                                    ////GPU
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				    size = dimk*size/trans[d].r;
				}
				else {
                                    ////GPU
			            fast_transpose(dimk, dimi, w1, w2);
				}
                                ////GPU
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
                         ////GPU
			   aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                         */
                    }
                }
                else condition[mu] = false;
            }
            //Tensor<R> * r1 = new Tensor<R>(r); 
            //Tensor<R> * r01 = new Tensor<R>(r0); 
            //std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, coeffs, argsdest, argstol, argsfac);
            //return t2;  
            std::tr1::tuple<bool*, Transformation**, Transformation**,
                        bool*, bool*, Q*, Level, keyT, double, double,
                        WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                        Tensor<T>, Tensor<T> > t2(condition, trans, trans2,
                                                  doit2, doit1, mufacs, n, argsdest, argstol, argsfac, 
                                                  coeffs, f, f0);
            return t2;
        }

        template <typename T>
        std::tr1::tuple<bool*, Transformation**, Transformation**,
                        bool*, bool*, Q*, Level, keyT, double, double,
                        WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                        Tensor<T>, Tensor<T>, SC*, keyT > 
        apply_computepreprocess2(std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > >& t1) const{
            
          typedef TENSOR_RESULT_TYPE(T,Q) resultT;
          typedef resultT R;
          typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

          /*const*/ keyT/*&*/ argskey = std::tr1::get<0>(t1);
          /*const*/ keyT/*&*/ argsd = std::tr1::get<1>(t1);
          keyT/*&*/ argsdest = std::tr1::get<2>(t1);
          double/*&*/ argstol = std::tr1::get<3>(t1);
          double/*&*/ argsfac = std::tr1::get<4>(t1);
          double/*&*/ argscnorm = std::tr1::get<5>(t1);
          /*const*/ Tensor<R>/*&*/ coeff = std::tr1::get<6>(t1);
          dcT/*&*/ coeffs = std::tr1::get<7>(t1);

          /*const*/ Key<NDIM>/*&*/ source = argskey;
          /*const*/ Key<NDIM>/*&*/ shift = argsd;
          double tol = argstol/argsfac/argscnorm;

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            //print("inlined_apply \n");
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);
            /*
            if (rank == 21){
               print("rank = ",rank," NDIM = ",NDIM);
            }
            */
            
            for (int mu = 0; mu < rank; mu++){
               for (int d = 0; d < NDIM; d++){
                 //MADNESS_ASSERT(op->muops[mu].ops[d]->Rnormf);
                 //MADNESS_ASSERT(op->muops[mu].ops[d]->R.ptr());
                 //MADNESS_ASSERT(op->muops[mu].ops[d]->T.ptr());
               }
            }
            
            

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k);
            Tensor<resultT> r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            //std::vector<Slice> s1(std::max<std::size_t>(2,NDIM),Slice(0,k-1))

            const Tensor<T> f0 = copy(coeff(s0));
            
            Level n = source.level();
            const Tensor<T>& f = *input;
            Transformation ** trans = new Transformation*[rank];
            Transformation ** trans2 = new Transformation*[rank];
            for (int mu = 0; mu < rank; mu++){
                trans[mu] = new Transformation[NDIM];
                trans2[mu] = new Transformation[NDIM];
            }

            bool* condition = new bool[rank];

            bool* doit2;
            bool* doit1;
            doit2 = new bool[rank];
            doit1 = new bool[rank];
            for (int mu = 0; mu < rank; mu++){
                doit2[mu] = false;
                doit1[mu] =  false;
            }
            
            Q* mufacs = new Q[rank];
 
	    const long twok = 2*k;
	    long break_even;
	    
            if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
            long break_even2;
            if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                mufacs[mu] = 0.0;
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    condition[mu] = true;
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    mufacs[mu] = mufac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0){
                        condition[mu] = false;
                        continue;
                    }

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[mu][d].r = twok;
			    trans[mu][d].U = ops[d]->R.ptr();
			    trans[mu][d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[mu][d].r = std::max(2L,r1);
			    trans[mu][d].U = ops[d]->RU.ptr();
			    trans[mu][d].VT = ops[d]->RVT.ptr();
			}
                    //print("CPUr_buffer[",mu,d,"] = ",trans[mu][d].r);
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		   
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

                    /*
		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }
                    */

		    // If all blocks are full rank we can skip the transposes
		    for (std::size_t d=0; d<NDIM; ++d) doit2[mu] = doit2[mu] || trans[mu][d].VT;

                    /*
		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
                                ////GPU
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
                                ////GPU
				fast_transpose(dimk, dimi, w1, w2);
			    }
                            ////GPU
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    */

		    if (n > 0) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans2[mu][d].r = k;
				trans2[mu][d].U = ops[d]->T.ptr();
				trans2[mu][d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans2[mu][d].r = std::max(2L,r1);
				trans2[mu][d].U = ops[d]->TU.ptr();
				trans2[mu][d].VT = ops[d]->TVT.ptr();
			    }
                        //print("CPUr_buffer[",mu,d,"] = ",trans2[mu][d].r);
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

                        /*
			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                        ////GPU
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                            ////GPU
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
                            ////GPU
		            std::swap(w1,w2);
			}
                        */

			// If all blocks are full rank we can skip the transposes
			for (std::size_t d=0; d<NDIM; ++d) doit1[mu] = doit1[mu] || trans2[mu][d].VT;

                        /*
			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
                                    ////GPU
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				   size = dimk*size/trans[d].r;
				}
				else {
                                    ////GPU
			            fast_transpose(dimk, dimi, w1, w2);
				}
                                ////GPU
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
                         ////GPU
			 aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                         */
                    }
                }
                else{ 
                   condition[mu] = false;
		   double Rnorm = 1.0;
		   for (std::size_t d=0; d<NDIM; ++d) Rnorm *= muop.ops[d]->Rnorm;
		   if (Rnorm != 0.0){
                    doit2[mu] = true;
                    doit1[mu] = true;
                   }
                }
            }
            //Tensor<R> * r1 = new Tensor<R>(r); 
            //Tensor<R> * r01 = new Tensor<R>(r0); 
            //std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, coeffs, argsdest, argstol, argsfac);
            //return t2;  
            std::tr1::tuple<bool*, Transformation**, Transformation**,
                        bool*, bool*, Q*, Level, keyT, double, double,
                        WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                        Tensor<T>, Tensor<T>, SC*, keyT > 
                                                  t2(condition, trans, trans2,
                                                  doit2, doit1, mufacs, n, argsdest, argstol, argsfac, 
                                                  coeffs, f, f0, const_cast<SC*>(op), shift );
            //print(n,shift);
            return t2;
        }

        template <typename T>
        Void apply_backToCPU(std::tr1::tuple<bool*, Transformation**, Transformation**,
                        bool*, bool*, Q*, Level, keyT, double, double,
                        WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                        Tensor<T>, Tensor<T>, SC*, keyT  > t) const{
            
          typedef TENSOR_RESULT_TYPE(T,Q) resultT;
          typedef resultT R;
          typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

            bool* condition = std::tr1::get<0>(t);
 
            Transformation ** trans = std::tr1::get<1>(t);
            Transformation ** trans2 = std::tr1::get<2>(t);

            bool* doit2 = std::tr1::get<3>(t);
            bool* doit1 = std::tr1::get<4>(t);

            Q* mufacs = std::tr1::get<5>(t);
            Level n = std::tr1::get<6>(t);
            keyT argsdest = std::tr1::get<7>(t);
            double argstol = std::tr1::get<8>(t);
            double argsfac = std::tr1::get<9>(t);
            dcT * coeffs = &(std::tr1::get<10>(t)); 
            Tensor<T> f = std::tr1::get<11>(t);
            Tensor<T> f0 = std::tr1::get<12>(t);          

            //SeparatedConvolutionData<Q, NDIM> * op_data = std::tr1::get<13>(*args[i]);
            keyT shift = std::tr1::get<14>(t);  
           
            Tensor<resultT> r(v2k);
            Tensor<resultT> r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

	    long dimk = 2*k;
	   
	    long size = 1;
	    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
	    long dimi = size/dimk;

	    long dimk2 = k;
	   
	    long size2 = 1;
	    for (std::size_t i=0; i<NDIM; ++i) size2 *= dimk2;
	    long dimi2 = size2/dimk2;

            const Q* U;
            const Q* U1;
            for (int mu=0; mu<rank; ++mu) {
              if (condition[mu]){
		    U = (trans[mu][0].r == dimk) ? trans[mu][0].U : shrink(dimk,dimk,trans[mu][0].r,trans[mu][0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[mu][0].r, dimk, w1, f.ptr(), U);
		    size = trans[mu][0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[mu][d].r == dimk) ? trans[mu][d].U : shrink(dimk,dimk,trans[mu][d].r,trans[mu][d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[mu][d].r, dimk, w2, w1, U);
			size = trans[mu][d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }

                    if (doit2[mu]){
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[mu][d].VT) {
				    dimi = size/trans[mu][d].r;
				    ////GPU
				    mTxmq(dimi, dimk, trans[mu][d].r, w2, w1, trans[mu][d].VT);
				    size = dimk*size/trans[mu][d].r;
				 }
				 else {
				    ////GPU
				    fast_transpose(dimk, dimi, w1, w2);
				 }
				 ////GPU
				 std::swap(w1,w2);
			    }
                    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, r.ptr(), w1, mufacs[mu]);

                    if (n > 0){
			    U1 = (trans2[mu][0].r == dimk2) ? trans2[mu][0].U : shrink(dimk2,dimk2,trans2[mu][0].r,trans2[mu][0].U,w3);
			    ////GPU
			    mTxmq(dimi2, trans2[mu][0].r, dimk2, w1, f0.ptr(), U1);
			    size2 = trans2[mu][0].r * size2 / dimk2;
			    dimi2 = size2/dimk2;
			    for (std::size_t d=1; d<NDIM; ++d) {
				U1 = (trans2[mu][d].r == dimk2) ? trans2[mu][d].U : shrink(dimk2,dimk2,trans2[mu][d].r,trans2[mu][d].U,w3);
				////GPU
				mTxmq(dimi2, trans2[mu][d].r, dimk2, w2, w1, U1);
				size2 = trans2[mu][d].r * size2 / dimk2;
				dimi2 = size2/dimk2;
				////GPU
				std::swap(w1,w2);
			    }

			    if (doit1[mu]) {
				for (std::size_t d=0; d<NDIM; ++d) {
				    if (trans2[mu][d].VT) {
					dimi2 = size2/trans2[mu][d].r;
					////GPU
					mTxmq(dimi2, dimk2, trans2[mu][d].r, w2, w1, trans2[mu][d].VT);
					size2 = dimk2*size2/trans2[mu][d].r;
				    }
				    else {
					////GPU
					fast_transpose(dimk2, dimi2, w1, w2);
				    }
				    ////GPU
				    std::swap(w1,w2);
				}
			     }
			     // Assuming here that result is contiguous and aligned
			     ////GPU
			     aligned_axpy(size2, r0.ptr(), w1, -mufacs[mu]);
                     }
              }
            }

            delete condition;
            for (int mu = 0; mu < rank; mu++){
                delete trans[mu];
                delete trans2[mu];
            }
            delete trans;
            delete trans2;
            delete doit2;
            delete doit1;
            delete mufacs;
            
            r(s0).gaxpy(1.0,r0,1.0);
            if (r.normf()> 0.3*argstol/argsfac) {
                coeffs->task(argsdest, &FunctionNode<T,NDIM>::accumulate, r, *coeffs, argsdest, TaskAttributes::hipri());
            }
        
            return None;
        }

        //op->opt_inlined_apply(args.key, args.d, c, args.tol/args.fac/args.cnorm);
        template <typename T>
        std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >&, keyT&, double&, double&> 
        apply_compute(std::tr1::tuple<keyT&, keyT&, keyT&, 
                                      double&, double&, double&, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>&, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >& >& t1) const{
            
          typedef TENSOR_RESULT_TYPE(T,Q) resultT;
          typedef resultT R;
          typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

          const keyT& argskey = std::tr1::get<0>(t1);
          const keyT& argsd = std::tr1::get<1>(t1);
          keyT& argsdest = std::tr1::get<2>(t1);
          double& argstol = std::tr1::get<3>(t1);
          double& argsfac = std::tr1::get<4>(t1);
          double& argscnorm = std::tr1::get<5>(t1);
          const Tensor<R>& coeff = std::tr1::get<6>(t1);
          dcT& coeffs = std::tr1::get<7>(t1);

          const Key<NDIM>& source = argskey;
          const Key<NDIM>& shift = argsd;
          double tol = argstol/argsfac/argscnorm;

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            //print("inlined_apply \n");
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k);
            Tensor<resultT> r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            const Tensor<T> f0 = copy(coeff(s0));
            
            Level n = source.level();
            const Tensor<T>& f = *input;
            Transformation trans[NDIM];
	    const long twok = 2*k;
	    long break_even;
	    
            if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
            long break_even2;
            if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0) continue;

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[d].r = twok;
			    trans[d].U = ops[d]->R.ptr();
			    trans[d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[d].r = std::max(2L,r1);
			    trans[d].U = ops[d]->RU.ptr();
			    trans[d].VT = ops[d]->RVT.ptr();
			}
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		   
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }

		    // If all blocks are full rank we can skip the transposes
		    bool doit = false;
		    for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
                                ////GPU
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
                                ////GPU
				fast_transpose(dimk, dimi, w1, w2);
			    }
                            ////GPU
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);

		    if (n > 0) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans[d].r = k;
				trans[d].U = ops[d]->T.ptr();
				trans[d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans[d].r = std::max(2L,r1);
				trans[d].U = ops[d]->TU.ptr();
				trans[d].VT = ops[d]->TVT.ptr();
			    }
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                        ////GPU
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                            ////GPU
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
                            ////GPU
		            std::swap(w1,w2);
			}

			// If all blocks are full rank we can skip the transposes
			bool doit = false;
			for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
                                    ////GPU
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				    size = dimk*size/trans[d].r;
				}
				else {
                                    ////GPU
			            fast_transpose(dimk, dimi, w1, w2);
				}
                                ////GPU
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
                         ////GPU
			 aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    }
                }
            }
            Tensor<R> * r1 = new Tensor<R>(r); 
            Tensor<R> * r01 = new Tensor<R>(r0); 
            std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT&, keyT&, double&, double&> t2(r1, r01, coeffs, argsdest, argstol, argsfac);
            return t2;  
        }

        template <typename T>
        std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> 
        apply_computecloser(std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > >& t1) const{
            
          typedef TENSOR_RESULT_TYPE(T,Q) resultT;
          typedef resultT R;
          typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

          const keyT argskey = std::tr1::get<0>(t1);
          const keyT argsd = std::tr1::get<1>(t1);
          keyT argsdest = std::tr1::get<2>(t1);
          double argstol = std::tr1::get<3>(t1);
          double argsfac = std::tr1::get<4>(t1);
          double argscnorm = std::tr1::get<5>(t1);
          const Tensor<R> coeff = std::tr1::get<6>(t1);
          dcT coeffs = std::tr1::get<7>(t1);

          const Key<NDIM>& source = argskey;
          const Key<NDIM>& shift = argsd;
          double tol = argstol/argsfac/argscnorm;

            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            //print("inlined_apply \n");
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k);
            Tensor<resultT> r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            const Tensor<T> f0 = copy(coeff(s0));
            
            Level n = source.level();
            const Tensor<T>& f = *input;
            Transformation trans[NDIM];
	    const long twok = 2*k;
	    long break_even;
	    
            if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
            long break_even2;
            if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0) continue;

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[d].r = twok;
			    trans[d].U = ops[d]->R.ptr();
			    trans[d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[d].r = std::max(2L,r1);
			    trans[d].U = ops[d]->RU.ptr();
			    trans[d].VT = ops[d]->RVT.ptr();
			}
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		   
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }

		    // If all blocks are full rank we can skip the transposes
		    bool doit = false;
		    for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
                                ////GPU
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
                                ////GPU
				fast_transpose(dimk, dimi, w1, w2);
			    }
                            ////GPU
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);

		    if (n > 0) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans[d].r = k;
				trans[d].U = ops[d]->T.ptr();
				trans[d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans[d].r = std::max(2L,r1);
				trans[d].U = ops[d]->TU.ptr();
				trans[d].VT = ops[d]->TVT.ptr();
			    }
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                        ////GPU
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                            ////GPU
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
                            ////GPU
		            std::swap(w1,w2);
			}

			// If all blocks are full rank we can skip the transposes
			bool doit = false;
			for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
                                    ////GPU
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				    size = dimk*size/trans[d].r;
				}
				else {
                                    ////GPU
			            fast_transpose(dimk, dimi, w1, w2);
				}
                                ////GPU
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
                         ////GPU
			 aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    }
                }
            }
            Tensor<R> * r1 = new Tensor<R>(r); 
            Tensor<R> * r01 = new Tensor<R>(r0); 
            std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, coeffs, argsdest, argstol, argsfac);
            return t2;  
        }

        #if APPLY_GPU > 0 
        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allComputeGPUIndKernels_Cublas2(std::vector<std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > > inArgs, 
                      std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print("      apply_allComputeGPU              ",inArgs.size());
            
            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg;
           typedef TENSOR_RESULT_TYPE(T,Q) resultT;
	   typedef resultT R;
	   typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;
            R* w1_array = 0; //allocate on GPU
            R* w2_array = 0; //allocate GPU
            Q* w5_array = 0; //do not use, because no shrink

	    R* rptr_arrayCPU; //transfer to GPU
            R* r0ptr_arrayCPU; //transfer to GPU
            T* f0ptr_arrayCPU; //transfer to GPU
            T* fptr_arrayCPU;  //transfer to GPU
            R* rptr_arrayGPU; //transfer CPU <-> GPU
            R* r0ptr_arrayGPU; //transfer CPU <-> GPU
            T* f0ptr_arrayGPU; //transfer to GPU
            T* fptr_arrayGPU;  //transfer to GPU

            unsigned int* w1_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w2_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w5_offarray = new unsigned int[inArgs.size()]; //not used, because no shrink
            
            unsigned int* rptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* r0ptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* f0ptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU
            unsigned int* fptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU

            unsigned int i;

            bool** condition;

            condition = new bool*[inArgs.size()];
           
            for (i = 0; i < inArgs.size(); i++){
              condition[i] = new bool[rank];
            }
            
            unsigned int w1_off = 0;
            unsigned int w2_off = 0;
            unsigned int w5_off = 0;
            
            unsigned int rptr_off = 0;
            unsigned int r0ptr_off = 0;
            unsigned int f0ptr_off = 0;
            unsigned int fptr_off = 0;

            print("-----------BATCH-----------------");
            print("k = ",k);
            print("rank = ",rank);

            std::tr1::tuple<keyT, keyT, keyT,
                  double, double, double,
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > ** args = new std::tr1::tuple<keyT, keyT, keyT,
                                                                                           double, double, double,
                                                                                           Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                                                                                           WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > *[inArgs.size()];

            //on CPU
            Tensor<R> * r_array = new Tensor<R>[inArgs.size()];
            Tensor<R> * r0_array = new Tensor<R>[inArgs.size()];
            double * argstol_array = new double[inArgs.size()]; 
            double * argsfac_array = new double[inArgs.size()]; 
            keyT * argsdest_array = new keyT[inArgs.size()];
            dcT ** coeffs_array = new dcT*[inArgs.size()]; 
            double * tol_array = new double[inArgs.size()]; 
            Tensor<T> * f_array = new Tensor<T>[inArgs.size()];
            Tensor<T> * f0_array = new Tensor<T>[inArgs.size()];           
 
            Transformation *** trans;
            Transformation *** trans2;
            trans = new Transformation**[inArgs.size()];
            trans2 = new Transformation**[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                  unsigned int j;
                  trans[i] = new Transformation*[rank];
                  trans2[i] = new Transformation*[rank];
                  for (j = 0; j < rank; j++){
                    trans[i][j] = new Transformation[NDIM];
                    trans2[i][j] = new Transformation[NDIM];
                  }
            }

	    const long twok = 2*k;
	    long break_even;
	   
            Q* transU_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2U_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
            Q* transVT_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2VT_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
 
	    if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
	    long break_even2;
	    if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            Level * n_array = new Level[inArgs.size()];

            Q** mufacs = new Q*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              mufacs[i] = new Q[rank];
            }

            bool** doit2;
            bool** doit1;
            doit2 = new bool*[inArgs.size()];
            doit1 = new bool*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              doit2[i] = new bool[rank];
              doit1[i] = new bool[rank];
            }
 
            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));

		  keyT& source = std::tr1::get<0>(*args[i]);
		  keyT& shift = std::tr1::get<1>(*args[i]);

	          argsdest_array[i] = std::tr1::get<2>(*args[i]);
	          argstol_array[i] = std::tr1::get<3>(*args[i]);
	          argsfac_array[i] = std::tr1::get<4>(*args[i]);
	          double argscnorm = std::tr1::get<5>(*args[i]);
	          Tensor<R>& coeff = std::tr1::get<6>(*args[i]);
	          coeffs_array[i] = &(std::tr1::get<7>(*args[i]));
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
	          const std::vector<Slice>& s0ref = inObj.at(i)->s0;

	          tol_array[i] = argstol_array[i]/argsfac_array[i]/argscnorm;

		  Tensor<resultT> r(v2kref);
		  Tensor<resultT> r0(vkref);
                  r_array[i] = r;
                  r0_array[i] = r0;
		  Tensor<resultT> work1(v2kref,false), work2(v2kref,false);
		  Tensor<Q> work5(2*k, 2*k);

		  const Tensor<T>* input = &coeff;
		  Tensor<T> dummy;

		  if (coeff.dim(0) == k) {
			// This processes leaf nodes with only scaling
			// coefficients ... FuncImpl::apply by default does not
			// apply the operator to these since for smoothing operators
			// it is not necessary.  It is necessary for operators such
			// as differentiation and time evolution and will also occur
			// if the application of the operator widens the tree.
			dummy = Tensor<T>(v2kref);
			dummy(s0ref) = coeff;
			input = &dummy;
		  }
		  else {
	              MADNESS_ASSERT(coeff.dim(0)==2*k);
		  }

                  tol_array[i] = tol_array[i]/rank;
 
		  const Tensor<T> f0 = copy(coeff(s0ref));
                  f0_array[i] = f0;
		    
		  const Tensor<T>& f = *input;
                  f_array[i] = f;
		
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	          Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		  
                  const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

		  Level n = source.level();
                  n_array[i] = n;
	          
                  for (int mu=0; mu<rank ; ++mu) {
		      const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
		      if (muop.norm > tol_array[i]) {
                        condition[i][mu] = true;
		        Q fac = inObj.at(i)->ops[mu].getfac(); //same for the same mu and SeparatedConvolution instance

		        //glue
		        const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
		        double tol1 = tol_array[i]/std::abs(fac);
		        const Q mufac = fac;
                        mufacs[i][mu] = fac;
		     
		        double Rnorm = 1.0;
		        for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		        if (Rnorm == 0.0){
                          condition[i][mu] = false;
                          continue;
                        }

		        tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		        // Determine rank of SVD to use or if to use the full matrix
		        for (std::size_t d=0; d<NDIM; ++d) {
			  long r1;
			  for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			  }
			  if (r1 >= break_even) {
			    trans[i][mu][d].r = twok;
			    
                            trans[i][mu][d].U = ops[d]->R.ptr();
                            memcpy(&transU_CPU[twok*twok * (i*rank*NDIM + mu*NDIM + d)], trans[i][mu][d].U, twok*twok * sizeof(Q));
			    
                            trans[i][mu][d].VT = 0;
			  }
			  else {
			    r1 += (r1&1L);
			    trans[i][mu][d].r = std::max(2L,r1);

			    trans[i][mu][d].U = ops[d]->RU.ptr();
                            memcpy(&transU_CPU[twok*twok * (i*rank*NDIM + mu*NDIM + d)], trans[i][mu][d].U, twok*twok * sizeof(Q));

			    trans[i][mu][d].VT = ops[d]->RVT.ptr();
                            memcpy(&transVT_CPU[twok*twok * (i*rank*NDIM + mu*NDIM + d)], trans[i][mu][d].VT, twok*twok * sizeof(Q));
			  }
		        }
		        ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

                            
                        // If all blocks are full rank we can skip the transposes
		        doit2[i][mu] = false;
		        for (std::size_t d=0; d<NDIM; ++d) doit2[i][mu] = doit2[i][mu] || trans[i][mu][d].VT; //move this out of the loop, calculate it in previous one

                        //trans2
		        if (n > 0) {

                          for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1< k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans2[i][mu][d].r = k; 

				trans2[i][mu][d].U = ops[d]->T.ptr();
                                memcpy(&trans2U_CPU[k*k * (i*rank*NDIM + mu*NDIM + d)], trans2[i][mu][d].U, k*k * sizeof(Q));

				trans2[i][mu][d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans2[i][mu][d].r = std::max(2L,r1);

				trans2[i][mu][d].U = ops[d]->TU.ptr();
                                memcpy(&trans2U_CPU[k*k * (i*rank*NDIM + mu*NDIM + d)], trans2[i][mu][d].U, k*k * sizeof(Q));

				trans2[i][mu][d].VT = ops[d]->TVT.ptr();
                                memcpy(&trans2VT_CPU[k*k * (i*rank*NDIM + mu*NDIM + d)], trans2[i][mu][d].VT, k*k * sizeof(Q));
			    }
			  }
			  
                          // If all blocks are full rank we can skip the transposes
			  doit1[i][mu] = false;
			  for (std::size_t d=0; d<NDIM; ++d) doit1[i][mu] = doit1[i][mu] || trans2[i][mu][d].VT;
			  
			  ////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
			  //const Tensor<T>& f1 = f0;
                        }
                      }
                      else condition[i][mu] = false;
                    } 
                

                  
                  w1_offarray[i] = w1_off;
                  w2_offarray[i] = w2_off;
                  w5_offarray[i] = w5_off;
                  
                  rptr_offarray[i] = rptr_off;
                  r0ptr_offarray[i] = r0ptr_off;
                  f0ptr_offarray[i] = f0ptr_off;
                  fptr_offarray[i] = fptr_off;

                  w1_off += work1.size();
                  w2_off += work2.size();
                  w5_off += work5.size();
                  
                  rptr_off += result.size();
                  r0ptr_off += result0.size();
                  f0ptr_off += f0.size();
                  fptr_off += f.size();
            }

            ////w1_array = new R[w1_off]; 
            ////w2_array = new R[w2_off]; 
            ////w5_array = new Q[w5_off]; 
            R* w1temp = new R[w1_off/inArgs.size()];
            w1_array = GPUtransfer_buffer(w1_array, w1_off, false);
            w2_array = GPUtransfer_buffer(w2_array, w2_off, false);
            w5_array = GPUtransfer_buffer(w5_array, w5_off, false);

            rptr_arrayCPU = new R[rptr_off];
            r0ptr_arrayCPU = new R[r0ptr_off];
            f0ptr_arrayCPU = new T[f0ptr_off];
            fptr_arrayCPU = new T[fptr_off];

STARTt_TIMER;
            Q* transU_GPU = GPUtransfer_buffer(transU_CPU, twok*twok * (NDIM*rank*inArgs.size()), true); 
            Q* trans2U_GPU = GPUtransfer_buffer(trans2U_CPU, k*k * (NDIM*rank*inArgs.size()), true); 
            Q* transVT_GPU = GPUtransfer_buffer(transVT_CPU, twok*twok * (NDIM*rank*inArgs.size()), true); 
            Q* trans2VT_GPU = GPUtransfer_buffer(trans2VT_CPU, k*k * (NDIM*rank*inArgs.size()), true); 
ENDt_TIMER("trans trans");                  

            for (i = 0; i < inArgs.size(); i++){
                  T* f0ptr = const_cast<T*>(f0_array[i].ptr());
                  T* fptr = const_cast<T*>(f_array[i].ptr());
			    
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	          Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];

                  R* resultptr = result.ptr();
                  R* result0ptr = result0.ptr();
	   
                  memcpy(rptr_arrayCPU + rptr_offarray[i], resultptr, result.size()*sizeof(R));
                  memcpy(r0ptr_arrayCPU + r0ptr_offarray[i], result0ptr, result0.size()*sizeof(R));
                  memcpy(f0ptr_arrayCPU + f0ptr_offarray[i], f0ptr, f0_array[i].size()*sizeof(T));
                  memcpy(fptr_arrayCPU + fptr_offarray[i], fptr, f_array[i].size()*sizeof(T));
            } 

            rptr_arrayGPU = GPUtransfer_buffer(rptr_arrayCPU, rptr_off, true); //both on CPU and GPU
            r0ptr_arrayGPU = GPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_off, true); //both on CPU and GPU
            f0ptr_arrayGPU = GPUtransfer_buffer(f0ptr_arrayCPU, f0ptr_off, true); //both on CPU and GPU
            fptr_arrayGPU = GPUtransfer_buffer(fptr_arrayCPU, fptr_off, true); //both on CPU and GPU
			    
            long dim2k, dimk;
	    dim2k = twok;
            dimk = k;

            long size = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size *= dim2k;
            long dimi = size/dim2k;

            long size2 = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size2 *= dimk;
            long dimi2 = size2/dimk;

            const Q* U;
            R** w1ptr = new R*[inArgs.size()];
	    R** w2ptr = new R*[inArgs.size()];
            T** f0ptr = new T*[inArgs.size()];
	    T** fptr = new T*[inArgs.size()];
            R** resultptr = new R*[inArgs.size()];
            R** result0ptr = new R*[inArgs.size()];
           
	    for (i = 0; i < inArgs.size(); i++){			    
	      //if (condition[i][mu]) {

		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		    w1ptr[i] = w1_array + w1_offarray[i];
		    w2ptr[i] = w2_array + w2_offarray[i];
		    f0ptr[i] = f0ptr_arrayGPU + f0ptr_offarray[i];
		    fptr[i] = fptr_arrayGPU + fptr_offarray[i];
		    resultptr[i] = rptr_arrayGPU + rptr_offarray[i];
		    result0ptr[i] = r0ptr_arrayGPU + r0ptr_offarray[i];
              //}
            }
		   
STARTt_TIMER;
            GPU_streams=streams_initialize(NUM_STREAMS, cublas_handle); 
            int conds = 0; 
            int conds2 = 0; 
            for (int mu=0; mu<rank; ++mu) {

	        for (i = 0; i < inArgs.size(); i++){			    
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                 }
	        
	        for (i = 0; i < inArgs.size(); i++){			    
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                 }
		  
                 for (std::size_t d=1; d<NDIM; ++d) {
		      for (i = 0; i < inArgs.size(); i++){			    
			if (condition[i][mu]) {
                          conds++;
			  U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + d);
			  ////GPU
			  cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			  ////GPU
			  std::swap(w1ptr[i],w2ptr[i]);
			}
		      }
		  }
		  
		  for (std::size_t d=0; d<NDIM; ++d) {
	            for (i = 0; i < inArgs.size(); i++){			    
			if (doit2[i][mu] & condition[i][mu]) {
			    if (trans[i][mu][d].VT) {
                                conds++;
				////GPU
				cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(transVT_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + d)), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    }
			    else {
				////GPU
				fast_transpose(dim2k, dimi, w1ptr[i], w2ptr[i]);
			    }
			    ////GPU
			    std::swap(w1ptr[i],w2ptr[i]);
			}
                      }
		    }

                    for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu]){   
			    cu_axpystream(size, resultptr[i], w1ptr[i], mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
                        }
                    }
                  }
            device_synchronize(GPU_streams,NUM_STREAMS);  
ENDt_TIMER("computation 1");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
	            
STARTt_TIMER;
            for (int mu=0; mu<rank; ++mu) {

	            for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
                            conds2++;
                            U = trans2U_GPU + k*k * (i*rank*NDIM + mu*NDIM + 0);
		            ////GPU
                            
		            cu_mTxmqnewstream(dimi2, dimk, dimk, w1ptr[i], f0ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                        }
                    }
	            
                    for (std::size_t d=1; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && n_array[i] > 0){
                                    conds2++;
                                    U = trans2U_GPU + k*k * (i*rank*NDIM + mu*NDIM + d);
				    ////GPU
				    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
				    ////GPU
                                    std::swap(w1ptr[i],w2ptr[i]);
			    }
                        }
                    }
	            
                    for (std::size_t d=0; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && doit1[i][mu] && n_array[i] > 0) {
					if (trans2[i][mu][d].VT) {
                                            conds2++;
					    ////GPU
					    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr[i], w1ptr[i], trans2VT_GPU + k*k * (i*rank*NDIM + mu*NDIM + d), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
					}
					else {
					    ////GPU
					    fast_transpose(dimk, dimi2, w1ptr[i], w2ptr[i]);
					}
					////GPU
                                        std::swap(w1ptr[i],w2ptr[i]);
		            }
		         }
                     }
	             
                     for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
				 ////GPU
				 cu_axpystream(size2, result0ptr[i], w1ptr[i], -mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
			}
                     }
//ENDt_TIMER("comp 8");
             }
            device_synchronize(GPU_streams,NUM_STREAMS);  
ENDt_TIMER("computation2");
print("conds2 = ",conds2," FLOP = ",((long)conds2)*20000);
            device_synchronize(GPU_streams,NUM_STREAMS);  
            
            CPUtransfer_buffer(rptr_arrayCPU, rptr_arrayGPU, rptr_off);
            CPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_arrayGPU, r0ptr_off);

            for (i = 0; i < inArgs.size(); i++){
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	            Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
                    R* resultptr = &rptr_arrayCPU[rptr_offarray[i]];
                    R* result0ptr = &r0ptr_arrayCPU[r0ptr_offarray[i]];
                    memcpy(result.ptr(), resultptr, result.size()*sizeof(R));
                    memcpy(result0.ptr(), result0ptr, result0.size()*sizeof(R));
		    Tensor<R> * r1 = new Tensor<R>(r_array[i]); 
		    Tensor<R> * r01 = new Tensor<R>(r0_array[i]); 
		    std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, *coeffs_array[i], argsdest_array[i], argstol_array[i], argsfac_array[i]);
                    outArg.push_back(t2);
            }

            GPUdelete_buffer(w1_array); //GPU 
            GPUdelete_buffer(w2_array); //GPU
            GPUdelete_buffer(w5_array); 

            delete[] rptr_arrayCPU; //CPU
            delete[] r0ptr_arrayCPU; //CPU
            delete[] f0ptr_arrayCPU; //CPU
            delete[] fptr_arrayCPU;  //CPU

            delete[] w1temp;
            delete[] w1ptr;
            delete[] w2ptr;
            delete[] fptr;
            delete[] f0ptr;
            delete[] resultptr;
            delete[] result0ptr;

            GPUdelete_buffer(rptr_arrayGPU); //GPU
            GPUdelete_buffer(r0ptr_arrayGPU); //GPU
            GPUdelete_buffer(f0ptr_arrayGPU); //GPU
            GPUdelete_buffer(fptr_arrayGPU);  //GPU

            delete[] args; 
            
            delete[] r_array;
            delete[] r0_array;
            delete[] argstol_array;
            delete[] argsfac_array;
            delete[] argsdest_array;
            delete[] coeffs_array;
            delete[] tol_array;
            delete[] f_array;
            delete[] f0_array;
           
            for (i = 0; i < inArgs.size(); i++){
              unsigned int j;
              for (j = 0; j < rank; j++){
                delete trans[i][j];
                delete trans2[i][j]; 
              }
              delete trans[i];
              delete trans2[i];
            }
            delete trans;
            delete trans2;

            delete transU_CPU;
            delete trans2U_CPU;
            delete transVT_CPU;
            delete trans2VT_CPU;

            GPUdelete_buffer(transU_GPU); //GPU
            GPUdelete_buffer(trans2U_GPU); //GPU
            GPUdelete_buffer(transVT_GPU); //GPU
            GPUdelete_buffer(trans2VT_GPU); //GPU

            for (i = 0; i < inArgs.size(); i++){
              delete doit2[i];
              delete doit1[i];
            }

            for (i = 0; i < inArgs.size(); i++){
              delete condition[i];
            } 
            delete condition;

            delete n_array;

            for (i = 0; i < inArgs.size(); i++){
              delete mufacs[i];
            } 
            delete mufacs;

            return outArg;
        }

        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allComputeGPUIndKernels_Cublas3(std::vector<std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > > inArgs, 
                      std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print("      apply_allComputeGPU              ",inArgs.size());
            
            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg;
           typedef TENSOR_RESULT_TYPE(T,Q) resultT;
	   typedef resultT R;
	   typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;
            R* w1_array = 0; //allocate on GPU
            R* w2_array = 0; //allocate GPU
            R* w1_array2 = 0; //allocate on GPU
            R* w2_array2 = 0; //allocate GPU
            Q* w5_array = 0; //do not use, because no shrink

	    R* rptr_arrayCPU; //transfer to GPU
            R* r0ptr_arrayCPU; //transfer to GPU
            T* f0ptr_arrayCPU; //transfer to GPU
            T* fptr_arrayCPU;  //transfer to GPU
            R* rptr_arrayGPU; //transfer CPU <-> GPU
            R* r0ptr_arrayGPU; //transfer CPU <-> GPU
            T* f0ptr_arrayGPU; //transfer to GPU
            T* fptr_arrayGPU;  //transfer to GPU

            unsigned int* w1_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w2_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w5_offarray = new unsigned int[inArgs.size()]; //not used, because no shrink
            
            unsigned int* rptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* r0ptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* f0ptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU
            unsigned int* fptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU

            unsigned int i;

            bool** condition;

            condition = new bool*[inArgs.size()];
           
            for (i = 0; i < inArgs.size(); i++){
              condition[i] = new bool[rank];
            }
            
            unsigned int w1_off = 0;
            unsigned int w2_off = 0;
            unsigned int w5_off = 0;
            
            unsigned int rptr_off = 0;
            unsigned int r0ptr_off = 0;
            unsigned int f0ptr_off = 0;
            unsigned int fptr_off = 0;

            print("-----------BATCH-----------------");
            print("k = ",k);
            print("rank = ",rank);

            std::tr1::tuple<keyT, keyT, keyT,
                  double, double, double,
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > ** args = new std::tr1::tuple<keyT, keyT, keyT,
                                                                                           double, double, double,
                                                                                           Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                                                                                           WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > *[inArgs.size()];

            //on CPU
            Tensor<R> * r_array = new Tensor<R>[inArgs.size()];
            Tensor<R> * r0_array = new Tensor<R>[inArgs.size()];
            double * argstol_array = new double[inArgs.size()]; 
            double * argsfac_array = new double[inArgs.size()]; 
            keyT * argsdest_array = new keyT[inArgs.size()];
            dcT ** coeffs_array = new dcT*[inArgs.size()]; 
            double * tol_array = new double[inArgs.size()]; 
            Tensor<T> * f_array = new Tensor<T>[inArgs.size()];
            Tensor<T> * f0_array = new Tensor<T>[inArgs.size()];           
 
            Transformation *** trans;
            Transformation *** trans2;
            trans = new Transformation**[inArgs.size()];
            trans2 = new Transformation**[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                  unsigned int j;
                  trans[i] = new Transformation*[rank];
                  trans2[i] = new Transformation*[rank];
                  for (j = 0; j < rank; j++){
                    trans[i][j] = new Transformation[NDIM];
                    trans2[i][j] = new Transformation[NDIM];
                  }
            }

	    const long twok = 2*k;
	    long break_even;
	   
            Q* transU_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2U_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
            Q* transVT_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2VT_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
 
	    if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
	    long break_even2;
	    if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            Level * n_array = new Level[inArgs.size()];

            Q** mufacs = new Q*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              mufacs[i] = new Q[rank];
            }

            bool** doit2;
            bool** doit1;
            doit2 = new bool*[inArgs.size()];
            doit1 = new bool*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              doit2[i] = new bool[rank];
              doit1[i] = new bool[rank];
            }
 
            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));

		  keyT& source = std::tr1::get<0>(*args[i]);
		  keyT& shift = std::tr1::get<1>(*args[i]);

	          argsdest_array[i] = std::tr1::get<2>(*args[i]);
	          argstol_array[i] = std::tr1::get<3>(*args[i]);
	          argsfac_array[i] = std::tr1::get<4>(*args[i]);
	          double argscnorm = std::tr1::get<5>(*args[i]);
	          Tensor<R>& coeff = std::tr1::get<6>(*args[i]);
	          coeffs_array[i] = &(std::tr1::get<7>(*args[i]));
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
	          const std::vector<Slice>& s0ref = inObj.at(i)->s0;

	          tol_array[i] = argstol_array[i]/argsfac_array[i]/argscnorm;

		  Tensor<resultT> r(v2kref);
		  Tensor<resultT> r0(vkref);
                  r_array[i] = r;
                  r0_array[i] = r0;
		  Tensor<resultT> work1(v2kref,false), work2(v2kref,false);
		  Tensor<Q> work5(2*k, 2*k);

		  const Tensor<T>* input = &coeff;
		  Tensor<T> dummy;

		  if (coeff.dim(0) == k) {
			// This processes leaf nodes with only scaling
			// coefficients ... FuncImpl::apply by default does not
			// apply the operator to these since for smoothing operators
			// it is not necessary.  It is necessary for operators such
			// as differentiation and time evolution and will also occur
			// if the application of the operator widens the tree.
			dummy = Tensor<T>(v2kref);
			dummy(s0ref) = coeff;
			input = &dummy;
		  }
		  else {
	              MADNESS_ASSERT(coeff.dim(0)==2*k);
		  }

                  tol_array[i] = tol_array[i]/rank;
 
		  const Tensor<T> f0 = copy(coeff(s0ref));
                  f0_array[i] = f0;
		    
		  const Tensor<T>& f = *input;
                  f_array[i] = f;
		
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	          Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		  
                  const SeparatedConvolutionData<Q,NDIM>* op = getopGPU(source.level(), shift);

                  if (apply_prev_offset < apply_curr_offset){
                    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, apply_buffer + apply_prev_offset, 
                                              apply_curr_offset - apply_prev_offset);
                    apply_prev_offset = apply_curr_offset;
                  }

		  Level n = source.level();
                  n_array[i] = n;
	          
                  for (int mu=0; mu<rank ; ++mu) {
		      const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
		      if (muop.norm > tol_array[i]) {
                        condition[i][mu] = true;
		        Q fac = inObj.at(i)->ops[mu].getfac(); //same for the same mu and SeparatedConvolution instance

		        //glue
		        const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
		        double tol1 = tol_array[i]/std::abs(fac);
		        const Q mufac = fac;
                        mufacs[i][mu] = fac;
		     
		        double Rnorm = 1.0;
		        for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		        if (Rnorm == 0.0){
                          condition[i][mu] = false;
                          continue;
                        }

		        tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		        // Determine rank of SVD to use or if to use the full matrix
		        for (std::size_t d=0; d<NDIM; ++d) {
			  long r1;
			  for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			  }
			  if (r1 >= break_even) {
			    trans[i][mu][d].r = twok;
			    
                            trans[i][mu][d].U = ops[d]->R.ptr();
                            memcpy(&transU_CPU[twok*twok * (i*rank*NDIM + mu*NDIM + d)], trans[i][mu][d].U, twok*twok * sizeof(Q));
                            trans[i][mu][d].U = ops[d]->GPUR;
                            MADNESS_ASSERT(trans[i][mu][d].U != 0);
			    
                            trans[i][mu][d].VT = 0;
			  }
			  else {
			    r1 += (r1&1L);
			    trans[i][mu][d].r = std::max(2L,r1);

			    trans[i][mu][d].U = ops[d]->RU.ptr();
                            memcpy(&transU_CPU[twok*twok * (i*rank*NDIM + mu*NDIM + d)], trans[i][mu][d].U, twok*twok * sizeof(Q));
			    trans[i][mu][d].U = ops[d]->GPURU;
                            MADNESS_ASSERT(trans[i][mu][d].U != 0);

			    trans[i][mu][d].VT = ops[d]->RVT.ptr();
                            memcpy(&transVT_CPU[twok*twok * (i*rank*NDIM + mu*NDIM + d)], trans[i][mu][d].VT, twok*twok * sizeof(Q));
			    trans[i][mu][d].VT = ops[d]->GPURVT;
                            MADNESS_ASSERT(trans[i][mu][d].VT != 0);
			  }
		        }
		        ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

                            
                        // If all blocks are full rank we can skip the transposes
		        doit2[i][mu] = false;
		        for (std::size_t d=0; d<NDIM; ++d) doit2[i][mu] = doit2[i][mu] || trans[i][mu][d].VT; //move this out of the loop, calculate it in previous one

                        //trans2
		        if (n > 0) {

                          for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1< k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans2[i][mu][d].r = k; 

				trans2[i][mu][d].U = ops[d]->T.ptr();
                                memcpy(&trans2U_CPU[k*k * (i*rank*NDIM + mu*NDIM + d)], trans2[i][mu][d].U, k*k * sizeof(Q));
				trans2[i][mu][d].U = ops[d]->GPUT;
                                MADNESS_ASSERT(trans[i][mu][d].U != 0);

				trans2[i][mu][d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans2[i][mu][d].r = std::max(2L,r1);

				trans2[i][mu][d].U = ops[d]->TU.ptr();
                                memcpy(&trans2U_CPU[k*k * (i*rank*NDIM + mu*NDIM + d)], trans2[i][mu][d].U, k*k * sizeof(Q));
				trans2[i][mu][d].U = ops[d]->GPUTU;
                                MADNESS_ASSERT(trans[i][mu][d].U != 0);

				trans2[i][mu][d].VT = ops[d]->TVT.ptr();
                                memcpy(&trans2VT_CPU[k*k * (i*rank*NDIM + mu*NDIM + d)], trans2[i][mu][d].VT, k*k * sizeof(Q));
				trans2[i][mu][d].VT = ops[d]->GPUTVT;
                                MADNESS_ASSERT(trans[i][mu][d].VT != 0);
			    }
			  }
			  
                          // If all blocks are full rank we can skip the transposes
			  doit1[i][mu] = false;
			  for (std::size_t d=0; d<NDIM; ++d) doit1[i][mu] = doit1[i][mu] || trans2[i][mu][d].VT;
			  
			  ////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
			  //const Tensor<T>& f1 = f0;
                        }
                      }
                      else condition[i][mu] = false;
                    } 
                

                  
                  w1_offarray[i] = w1_off;
                  w2_offarray[i] = w2_off;
                  w5_offarray[i] = w5_off;
                  
                  rptr_offarray[i] = rptr_off;
                  r0ptr_offarray[i] = r0ptr_off;
                  f0ptr_offarray[i] = f0ptr_off;
                  fptr_offarray[i] = fptr_off;

                  w1_off += work1.size();
                  w2_off += work2.size();
                  w5_off += work5.size();
                  
                  rptr_off += result.size();
                  r0ptr_off += result0.size();
                  f0ptr_off += f0.size();
                  fptr_off += f.size();
            }

            ////w1_array = new R[w1_off]; 
            ////w2_array = new R[w2_off]; 
            ////w5_array = new Q[w5_off]; 
            R* w1temp = new R[w1_off/inArgs.size()];
            R* w1temp2 = new R[w1_off/inArgs.size()];
            w1_array = GPUtransfer_buffer(w1_array, w1_off, false);
            w2_array = GPUtransfer_buffer(w2_array, w2_off, false);
            w1_array2 = GPUtransfer_buffer(w1_array2, w1_off, false);
            w2_array2 = GPUtransfer_buffer(w2_array2, w2_off, false);
            w5_array = GPUtransfer_buffer(w5_array, w5_off, false);

            rptr_arrayCPU = new R[rptr_off];
            r0ptr_arrayCPU = new R[r0ptr_off];
            f0ptr_arrayCPU = new T[f0ptr_off];
            fptr_arrayCPU = new T[fptr_off];

STARTt_TIMER;
            Q* transU_GPU = GPUtransfer_buffer(transU_CPU, twok*twok * (NDIM*rank*inArgs.size()), true); 
            Q* trans2U_GPU = GPUtransfer_buffer(trans2U_CPU, k*k * (NDIM*rank*inArgs.size()), true); 
            Q* transVT_GPU = GPUtransfer_buffer(transVT_CPU, twok*twok * (NDIM*rank*inArgs.size()), true); 
            Q* trans2VT_GPU = GPUtransfer_buffer(trans2VT_CPU, k*k * (NDIM*rank*inArgs.size()), true); 
ENDt_TIMER("trans trans");                  

            for (i = 0; i < inArgs.size(); i++){
                  T* f0ptr = const_cast<T*>(f0_array[i].ptr());
                  T* fptr = const_cast<T*>(f_array[i].ptr());
			    
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	          Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];

                  R* resultptr = result.ptr();
                  R* result0ptr = result0.ptr();
	   
                  memcpy(rptr_arrayCPU + rptr_offarray[i], resultptr, result.size()*sizeof(R));
                  memcpy(r0ptr_arrayCPU + r0ptr_offarray[i], result0ptr, result0.size()*sizeof(R));
                  memcpy(f0ptr_arrayCPU + f0ptr_offarray[i], f0ptr, f0_array[i].size()*sizeof(T));
                  memcpy(fptr_arrayCPU + fptr_offarray[i], fptr, f_array[i].size()*sizeof(T));
            } 

            rptr_arrayGPU = GPUtransfer_buffer(rptr_arrayCPU, rptr_off, true); //both on CPU and GPU
            r0ptr_arrayGPU = GPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_off, true); //both on CPU and GPU
            f0ptr_arrayGPU = GPUtransfer_buffer(f0ptr_arrayCPU, f0ptr_off, true); //both on CPU and GPU
            fptr_arrayGPU = GPUtransfer_buffer(fptr_arrayCPU, fptr_off, true); //both on CPU and GPU
			    
            long dim2k, dimk;
	    dim2k = twok;
            dimk = k;

            long size = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size *= dim2k;
            long dimi = size/dim2k;

            long size2 = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size2 *= dimk;
            long dimi2 = size2/dimk;

            const Q* U;
            R** w1ptr = new R*[inArgs.size()];
	    R** w2ptr = new R*[inArgs.size()];
            R** w1ptr2 = new R*[inArgs.size()];
	    R** w2ptr2 = new R*[inArgs.size()];
            T** f0ptr = new T*[inArgs.size()];
	    T** fptr = new T*[inArgs.size()];
            R** resultptr = new R*[inArgs.size()];
            R** result0ptr = new R*[inArgs.size()];
           
	    for (i = 0; i < inArgs.size(); i++){			    
	      //if (condition[i][mu]) {

		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		    w1ptr[i] = w1_array + w1_offarray[i];
		    w2ptr[i] = w2_array + w2_offarray[i];
		    w1ptr2[i] = w1_array2 + w1_offarray[i];
		    w2ptr2[i] = w2_array2 + w2_offarray[i];
		    f0ptr[i] = f0ptr_arrayGPU + f0ptr_offarray[i];
		    fptr[i] = fptr_arrayGPU + fptr_offarray[i];
		    resultptr[i] = rptr_arrayGPU + rptr_offarray[i];
		    result0ptr[i] = r0ptr_arrayGPU + r0ptr_offarray[i];
              //}
            }
		   
            int conds = 0; 
            int conds2 = 0;
/*
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 hunred one write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000*100);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all one write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralOptCWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all optC write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralOneNoWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all one nowrite tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integral11tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all 1 tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integral1tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 1 tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000*100);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integral111tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 1 no write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000*100);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 Cublas tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
*/
conds = 0;
STARTt_TIMER;
            GPU_streams=streams_initialize(NUM_STREAMS, cublas_handle); 
            for (int mu=0; mu<rank; ++mu) {

	        for (i = 0; i < inArgs.size(); i++){			    
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);
                            U = trans[i][mu][0].U;

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                 }
	        
	            for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
                            conds2++;
                            U = trans2U_GPU + k*k * (i*rank*NDIM + mu*NDIM + 0);
                            U = trans2[i][mu][0].U;
		            ////GPU
                            
		            cu_mTxmqnewstream(dimi2, dimk, dimk, w1ptr2[i], f0ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                        }
                    }
	            
                 for (std::size_t d=1; d<NDIM; ++d) {
		      for (i = 0; i < inArgs.size(); i++){			    
			if (condition[i][mu]) {
                          conds++;
			  U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + d);
                          U = trans[i][mu][d].U;
			  ////GPU
			  cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			  ////GPU
			  std::swap(w1ptr[i],w2ptr[i]);
			}
		      }
		  }
		  
                    for (std::size_t d=1; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && n_array[i] > 0){
                                    conds2++;
                                    U = trans2U_GPU + k*k * (i*rank*NDIM + mu*NDIM + d);
                                    U = trans2[i][mu][d].U;
				    ////GPU
				    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
				    ////GPU
                                    std::swap(w1ptr2[i],w2ptr2[i]);
			    }
                        }
                    }
	            
		  for (std::size_t d=0; d<NDIM; ++d) {
	            for (i = 0; i < inArgs.size(); i++){			    
			if (doit2[i][mu] & condition[i][mu]) {
			    if (trans[i][mu][d].VT) {
                                conds++;
                                U = transVT_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + d);
                                U = trans[i][mu][d].VT;
				////GPU
				cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    }
			    else {
				////GPU
				fast_transpose(dim2k, dimi, w1ptr[i], w2ptr[i]);
			    }
			    ////GPU
			    std::swap(w1ptr[i],w2ptr[i]);
			}
                      }
		    }
                    
                    for (std::size_t d=0; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && doit1[i][mu] && n_array[i] > 0) {
					if (trans2[i][mu][d].VT) {
                                            U = trans2VT_GPU + k*k * (i*rank*NDIM + mu*NDIM + d);
                                            U = trans2[i][mu][d].VT;
                                            conds2++;
					    ////GPU
					    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
					}
					else {
					    ////GPU
					    fast_transpose(dimk, dimi2, w1ptr2[i], w2ptr2[i]);
					}
					////GPU
                                        std::swap(w1ptr2[i],w2ptr2[i]);
		            }
		         }
                     }
	             
                     for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
				 ////GPU
				 cu_axpystream(size2, result0ptr[i], w1ptr2[i], -mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
			}
                     }
                    for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu]){   
			    cu_axpystream(size, resultptr[i], w1ptr[i], mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
                        }
                    }

                  }
            device_synchronize(GPU_streams,NUM_STREAMS);  
ENDt_TIMER("computation 1");
print("conds = ",conds," conds2 = "," FLOP = ",((long)conds)*320000 + ((long)conds2)*20000);
	            
print("conds2 = ",conds2," FLOP = ",((long)conds2)*20000);
            device_synchronize(GPU_streams,NUM_STREAMS);  
            
            CPUtransfer_buffer(rptr_arrayCPU, rptr_arrayGPU, rptr_off);
            CPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_arrayGPU, r0ptr_off);

            for (i = 0; i < inArgs.size(); i++){
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	            Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
                    R* resultptr = &rptr_arrayCPU[rptr_offarray[i]];
                    R* result0ptr = &r0ptr_arrayCPU[r0ptr_offarray[i]];
                    memcpy(result.ptr(), resultptr, result.size()*sizeof(R));
                    memcpy(result0.ptr(), result0ptr, result0.size()*sizeof(R));
		    Tensor<R> * r1 = new Tensor<R>(r_array[i]); 
		    Tensor<R> * r01 = new Tensor<R>(r0_array[i]); 
		    std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, *coeffs_array[i], argsdest_array[i], argstol_array[i], argsfac_array[i]);
                    outArg.push_back(t2);
            }

            GPUdelete_buffer(w1_array); //GPU 
            GPUdelete_buffer(w2_array); //GPU
            GPUdelete_buffer(w1_array2); //GPU 
            GPUdelete_buffer(w2_array2); //GPU
            GPUdelete_buffer(w5_array); 

            delete[] rptr_arrayCPU; //CPU
            delete[] r0ptr_arrayCPU; //CPU
            delete[] f0ptr_arrayCPU; //CPU
            delete[] fptr_arrayCPU;  //CPU

            delete[] w1temp;
            delete[] w1temp2;
            delete[] w1ptr;
            delete[] w2ptr;
            delete[] w1ptr2;
            delete[] w2ptr2;
            delete[] fptr;
            delete[] f0ptr;
            delete[] resultptr;
            delete[] result0ptr;

            GPUdelete_buffer(rptr_arrayGPU); //GPU
            GPUdelete_buffer(r0ptr_arrayGPU); //GPU
            GPUdelete_buffer(f0ptr_arrayGPU); //GPU
            GPUdelete_buffer(fptr_arrayGPU);  //GPU

            delete[] args; 
            
            delete[] r_array;
            delete[] r0_array;
            delete[] argstol_array;
            delete[] argsfac_array;
            delete[] argsdest_array;
            delete[] coeffs_array;
            delete[] tol_array;
            delete[] f_array;
            delete[] f0_array;
           
            for (i = 0; i < inArgs.size(); i++){
              unsigned int j;
              for (j = 0; j < rank; j++){
                delete trans[i][j];
                delete trans2[i][j]; 
              }
              delete trans[i];
              delete trans2[i];
            }
            delete trans;
            delete trans2;

            delete transU_CPU;
            delete trans2U_CPU;
            delete transVT_CPU;
            delete trans2VT_CPU;

            GPUdelete_buffer(transU_GPU); //GPU
            GPUdelete_buffer(trans2U_GPU); //GPU
            GPUdelete_buffer(transVT_GPU); //GPU
            GPUdelete_buffer(trans2VT_GPU); //GPU

            for (i = 0; i < inArgs.size(); i++){
              delete doit2[i];
              delete doit1[i];
            }

            for (i = 0; i < inArgs.size(); i++){
              delete condition[i];
            } 
            delete condition;

            delete n_array;

            for (i = 0; i < inArgs.size(); i++){
              delete mufacs[i];
            } 
            delete mufacs;

            return outArg;
        }
 
        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allComputeGPUIndKernels_Cublas4(std::vector<std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > > inArgs, 
                      std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print("      apply_allComputeGPU              ",inArgs.size());
            
            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg;
           typedef TENSOR_RESULT_TYPE(T,Q) resultT;
	   typedef resultT R;
	   typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;
            R* w1_array = 0; //allocate on GPU
            R* w2_array = 0; //allocate GPU
            R* w1_array2 = 0; //allocate on GPU
            R* w2_array2 = 0; //allocate GPU
            Q* w5_array = 0; //do not use, because no shrink

	    R* rptr_arrayCPU; //transfer to GPU
            R* r0ptr_arrayCPU; //transfer to GPU
            T* f0ptr_arrayCPU; //transfer to GPU
            T* fptr_arrayCPU;  //transfer to GPU
            R* rptr_arrayGPU; //transfer CPU <-> GPU
            R* r0ptr_arrayGPU; //transfer CPU <-> GPU
            T* f0ptr_arrayGPU; //transfer to GPU
            T* fptr_arrayGPU;  //transfer to GPU

            unsigned int* w1_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w2_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w5_offarray = new unsigned int[inArgs.size()]; //not used, because no shrink
            
            unsigned int* rptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* r0ptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* f0ptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU
            unsigned int* fptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU

            unsigned int i;

            bool** condition;

            condition = new bool*[inArgs.size()];
           
            for (i = 0; i < inArgs.size(); i++){
              condition[i] = new bool[rank];
            }
            
            unsigned int w1_off = 0;
            unsigned int w2_off = 0;
            unsigned int w5_off = 0;
            
            unsigned int rptr_off = 0;
            unsigned int r0ptr_off = 0;
            unsigned int f0ptr_off = 0;
            unsigned int fptr_off = 0;

            print("-----------BATCH-----------------");
            print("k = ",k);
            print("rank = ",rank);

            std::tr1::tuple<keyT, keyT, keyT,
                  double, double, double,
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > ** args = new std::tr1::tuple<keyT, keyT, keyT,
                                                                                           double, double, double,
                                                                                           Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                                                                                           WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > *[inArgs.size()];

            //on CPU
            Tensor<R> * r_array = new Tensor<R>[inArgs.size()];
            Tensor<R> * r0_array = new Tensor<R>[inArgs.size()];
            double * argstol_array = new double[inArgs.size()]; 
            double * argsfac_array = new double[inArgs.size()]; 
            keyT * argsdest_array = new keyT[inArgs.size()];
            dcT ** coeffs_array = new dcT*[inArgs.size()]; 
            double * tol_array = new double[inArgs.size()]; 
            Tensor<T> * f_array = new Tensor<T>[inArgs.size()];
            Tensor<T> * f0_array = new Tensor<T>[inArgs.size()];           
 
            Transformation *** trans;
            Transformation *** trans2;
            trans = new Transformation**[inArgs.size()];
            trans2 = new Transformation**[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                  unsigned int j;
                  trans[i] = new Transformation*[rank];
                  trans2[i] = new Transformation*[rank];
                  for (j = 0; j < rank; j++){
                    trans[i][j] = new Transformation[NDIM];
                    trans2[i][j] = new Transformation[NDIM];
                  }
            }

	    const long twok = 2*k;
	    long break_even;
	   
            Q* transU_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2U_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
            Q* transVT_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2VT_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
 
	    if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
	    long break_even2;
	    if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            Level * n_array = new Level[inArgs.size()];

            Q** mufacs = new Q*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              mufacs[i] = new Q[rank];
            }

            bool** doit2;
            bool** doit1;
            doit2 = new bool*[inArgs.size()];
            doit1 = new bool*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              doit2[i] = new bool[rank];
              doit1[i] = new bool[rank];
            }
 
            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));

		  keyT& source = std::tr1::get<0>(*args[i]);
		  keyT& shift = std::tr1::get<1>(*args[i]);

	          argsdest_array[i] = std::tr1::get<2>(*args[i]);
	          argstol_array[i] = std::tr1::get<3>(*args[i]);
	          argsfac_array[i] = std::tr1::get<4>(*args[i]);
	          double argscnorm = std::tr1::get<5>(*args[i]);
	          Tensor<R>& coeff = std::tr1::get<6>(*args[i]);
	          coeffs_array[i] = &(std::tr1::get<7>(*args[i]));
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
	          const std::vector<Slice>& s0ref = inObj.at(i)->s0;

	          tol_array[i] = argstol_array[i]/argsfac_array[i]/argscnorm;

		  Tensor<resultT> r(v2kref);
		  Tensor<resultT> r0(vkref);
                  r_array[i] = r;
                  r0_array[i] = r0;
		  Tensor<resultT> work1(v2kref,false), work2(v2kref,false);
		  Tensor<Q> work5(2*k, 2*k);

		  const Tensor<T>* input = &coeff;
		  Tensor<T> dummy;

		  if (coeff.dim(0) == k) {
			// This processes leaf nodes with only scaling
			// coefficients ... FuncImpl::apply by default does not
			// apply the operator to these since for smoothing operators
			// it is not necessary.  It is necessary for operators such
			// as differentiation and time evolution and will also occur
			// if the application of the operator widens the tree.
			dummy = Tensor<T>(v2kref);
			dummy(s0ref) = coeff;
			input = &dummy;
		  }
		  else {
	              MADNESS_ASSERT(coeff.dim(0)==2*k);
		  }

                  tol_array[i] = tol_array[i]/rank;
 
		  const Tensor<T> f0 = copy(coeff(s0ref));
                  f0_array[i] = f0;
		    
		  const Tensor<T>& f = *input;
                  f_array[i] = f;
		
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	          Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		  
                  const SeparatedConvolutionData<Q,NDIM>* op = getopGPU(source.level(), shift);

                  if (apply_prev_offset < apply_curr_offset){
                    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, apply_buffer + apply_prev_offset, 
                                              apply_curr_offset - apply_prev_offset);
                    apply_prev_offset = apply_curr_offset;
                  }

		  Level n = source.level();
                  n_array[i] = n;
	          
                  for (int mu=0; mu<rank ; ++mu) {
		      const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
		      if (muop.norm > tol_array[i]) {
                        condition[i][mu] = true;
		        Q fac = inObj.at(i)->ops[mu].getfac(); //same for the same mu and SeparatedConvolution instance

		        //glue
		        const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
		        double tol1 = tol_array[i]/std::abs(fac);
		        const Q mufac = fac;
                        mufacs[i][mu] = fac;
		     
		        double Rnorm = 1.0;
		        for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		        if (Rnorm == 0.0){
                          condition[i][mu] = false;
                          continue;
                        }

		        tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		        // Determine rank of SVD to use or if to use the full matrix
		        for (std::size_t d=0; d<NDIM; ++d) {
			  long r1;
			  for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			  }
			  if (r1 >= break_even) {
			    trans[i][mu][d].r = twok;
			    
                            trans[i][mu][d].U = ops[d]->GPUR;
                            //MADNESS_ASSERT(trans[i][mu][d].U != 0);
			    
                            trans[i][mu][d].VT = 0;
			  }
			  else {
			    r1 += (r1&1L);
			    trans[i][mu][d].r = std::max(2L,r1);

			    trans[i][mu][d].U = ops[d]->GPURU;
                            //MADNESS_ASSERT(trans[i][mu][d].U != 0);

			    trans[i][mu][d].VT = ops[d]->GPURVT;
                            //MADNESS_ASSERT(trans[i][mu][d].VT != 0);
			  }
		        }
		        ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

                            
                        // If all blocks are full rank we can skip the transposes
		        doit2[i][mu] = false;
		        for (std::size_t d=0; d<NDIM; ++d) doit2[i][mu] = doit2[i][mu] || trans[i][mu][d].VT; //move this out of the loop, calculate it in previous one

                        //trans2
		        if (n > 0) {

                          for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1< k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans2[i][mu][d].r = k; 

				trans2[i][mu][d].U = ops[d]->GPUT;
                                //MADNESS_ASSERT(trans[i][mu][d].U != 0);

				trans2[i][mu][d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans2[i][mu][d].r = std::max(2L,r1);

				trans2[i][mu][d].U = ops[d]->GPUTU;
                                //MADNESS_ASSERT(trans[i][mu][d].U != 0);

				trans2[i][mu][d].VT = ops[d]->GPUTVT;
                                //MADNESS_ASSERT(trans[i][mu][d].VT != 0);
			    }
			  }
			  
                          // If all blocks are full rank we can skip the transposes
			  doit1[i][mu] = false;
			  for (std::size_t d=0; d<NDIM; ++d) doit1[i][mu] = doit1[i][mu] || trans2[i][mu][d].VT;
			  
			  ////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
			  //const Tensor<T>& f1 = f0;
                        }
                      }
                      else condition[i][mu] = false;
                    } 
                

                  
                  w1_offarray[i] = w1_off;
                  w2_offarray[i] = w2_off;
                  w5_offarray[i] = w5_off;
                  
                  rptr_offarray[i] = rptr_off;
                  r0ptr_offarray[i] = r0ptr_off;
                  f0ptr_offarray[i] = f0ptr_off;
                  fptr_offarray[i] = fptr_off;

                  w1_off += work1.size();
                  w2_off += work2.size();
                  w5_off += work5.size();
                  
                  rptr_off += result.size();
                  r0ptr_off += result0.size();
                  f0ptr_off += f0.size();
                  fptr_off += f.size();
            }

            ////w1_array = new R[w1_off]; 
            ////w2_array = new R[w2_off]; 
            ////w5_array = new Q[w5_off]; 
            R* w1temp = new R[w1_off/inArgs.size()];
            R* w1temp2 = new R[w1_off/inArgs.size()];
            w1_array = GPUtransfer_buffer(w1_array, w1_off, false);
            w2_array = GPUtransfer_buffer(w2_array, w2_off, false);
            w1_array2 = GPUtransfer_buffer(w1_array2, w1_off, false);
            w2_array2 = GPUtransfer_buffer(w2_array2, w2_off, false);
            w5_array = GPUtransfer_buffer(w5_array, w5_off, false);

            rptr_arrayCPU = new R[rptr_off];
            r0ptr_arrayCPU = new R[r0ptr_off];
            f0ptr_arrayCPU = new T[f0ptr_off];
            fptr_arrayCPU = new T[fptr_off];

/*
STARTt_TIMER;
            Q* transU_GPU = GPUtransfer_buffer(transU_CPU, twok*twok * (NDIM*rank*inArgs.size()), true); 
            Q* trans2U_GPU = GPUtransfer_buffer(trans2U_CPU, k*k * (NDIM*rank*inArgs.size()), true); 
            Q* transVT_GPU = GPUtransfer_buffer(transVT_CPU, twok*twok * (NDIM*rank*inArgs.size()), true); 
            Q* trans2VT_GPU = GPUtransfer_buffer(trans2VT_CPU, k*k * (NDIM*rank*inArgs.size()), true); 
ENDt_TIMER("trans trans");                  
*/

            for (i = 0; i < inArgs.size(); i++){
                  T* f0ptr = const_cast<T*>(f0_array[i].ptr());
                  T* fptr = const_cast<T*>(f_array[i].ptr());
			    
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	          Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];

                  R* resultptr = result.ptr();
                  R* result0ptr = result0.ptr();
	   
                  memcpy(rptr_arrayCPU + rptr_offarray[i], resultptr, result.size()*sizeof(R));
                  memcpy(r0ptr_arrayCPU + r0ptr_offarray[i], result0ptr, result0.size()*sizeof(R));
                  memcpy(f0ptr_arrayCPU + f0ptr_offarray[i], f0ptr, f0_array[i].size()*sizeof(T));
                  memcpy(fptr_arrayCPU + fptr_offarray[i], fptr, f_array[i].size()*sizeof(T));
            } 

            rptr_arrayGPU = GPUtransfer_buffer(rptr_arrayCPU, rptr_off, true); //both on CPU and GPU
            r0ptr_arrayGPU = GPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_off, true); //both on CPU and GPU
            f0ptr_arrayGPU = GPUtransfer_buffer(f0ptr_arrayCPU, f0ptr_off, true); //both on CPU and GPU
            fptr_arrayGPU = GPUtransfer_buffer(fptr_arrayCPU, fptr_off, true); //both on CPU and GPU
			    
            long dim2k, dimk;
	    dim2k = twok;
            dimk = k;

            long size = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size *= dim2k;
            long dimi = size/dim2k;

            long size2 = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size2 *= dimk;
            long dimi2 = size2/dimk;

            const Q* U;
            R** w1ptr = new R*[inArgs.size()];
	    R** w2ptr = new R*[inArgs.size()];
            R** w1ptr2 = new R*[inArgs.size()];
	    R** w2ptr2 = new R*[inArgs.size()];
            T** f0ptr = new T*[inArgs.size()];
	    T** fptr = new T*[inArgs.size()];
            R** resultptr = new R*[inArgs.size()];
            R** result0ptr = new R*[inArgs.size()];
           
	    for (i = 0; i < inArgs.size(); i++){			    
	      //if (condition[i][mu]) {

		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		    w1ptr[i] = w1_array + w1_offarray[i];
		    w2ptr[i] = w2_array + w2_offarray[i];
		    w1ptr2[i] = w1_array2 + w1_offarray[i];
		    w2ptr2[i] = w2_array2 + w2_offarray[i];
		    f0ptr[i] = f0ptr_arrayGPU + f0ptr_offarray[i];
		    fptr[i] = fptr_arrayGPU + fptr_offarray[i];
		    resultptr[i] = rptr_arrayGPU + rptr_offarray[i];
		    result0ptr[i] = r0ptr_arrayGPU + r0ptr_offarray[i];
              //}
            }
		   
            int conds = 0; 
            int conds2 = 0;
/*
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 hunred one write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000*100);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all one write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralOptCWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all optC write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integralOneNoWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all one nowrite tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integral11tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 all 1 tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integral1tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 1 tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000*100);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmq_integral111tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 1 no write tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000*100);
conds = 0;
STARTt_TIMER; 
            for (int mu=0; mu<rank; ++mu) {
                //for (int ik = 0; ik < 4; ik++){
	        for (i = 0; i < inArgs.size(); i++){			    
	             //if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
			    U = transU_GPU + twok*twok * (i*rank*NDIM + mu*NDIM + 0);

                            ////GPU
			    //cu_mTxmqnew(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                     //}
                 }
                 //}
             }
                 device_synchronize(GPU_streams,NUM_STREAMS);
print("conds = ",conds,"avg = ",1.0*conds/inArgs.size());
ENDt_TIMER("comp 1 Cublas tb");
print("conds = ",conds," FLOP = ",((long)conds)*320000);
*/
conds = 0;
STARTt_TIMER;
            GPU_streams=streams_initialize(NUM_STREAMS, cublas_handle); 
            for (int mu=0; mu<rank; ++mu) {

	        for (i = 0; i < inArgs.size(); i++){			    
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                            U = trans[i][mu][0].U;

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                 }
	        
	            for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
                            conds2++;
                            U = trans2[i][mu][0].U;
		            ////GPU
                            
		            cu_mTxmqnewstream(dimi2, dimk, dimk, w1ptr2[i], f0ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                        }
                    }
	            
                 for (std::size_t d=1; d<NDIM; ++d) {
		      for (i = 0; i < inArgs.size(); i++){			    
			if (condition[i][mu]) {
                          conds++;
                          U = trans[i][mu][d].U;
			  ////GPU
			  cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			  ////GPU
			  std::swap(w1ptr[i],w2ptr[i]);
			}
		      }
		  }
		  
                    for (std::size_t d=1; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && n_array[i] > 0){
                                    conds2++;
                                    U = trans2[i][mu][d].U;
				    ////GPU
				    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
				    ////GPU
                                    std::swap(w1ptr2[i],w2ptr2[i]);
			    }
                        }
                    }
	            
		  for (std::size_t d=0; d<NDIM; ++d) {
	            for (i = 0; i < inArgs.size(); i++){			    
			if (doit2[i][mu] & condition[i][mu]) {
			    if (trans[i][mu][d].VT) {
                                conds++;
                                U = trans[i][mu][d].VT;
				////GPU
				cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    }
			    else {
				////GPU
				fast_transpose(dim2k, dimi, w1ptr[i], w2ptr[i]);
			    }
			    ////GPU
			    std::swap(w1ptr[i],w2ptr[i]);
			}
                      }
		    }
                    
                    for (std::size_t d=0; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && doit1[i][mu] && n_array[i] > 0) {
					if (trans2[i][mu][d].VT) {
                                            U = trans2[i][mu][d].VT;
                                            conds2++;
					    ////GPU
					    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
					}
					else {
					    ////GPU
					    fast_transpose(dimk, dimi2, w1ptr2[i], w2ptr2[i]);
					}
					////GPU
                                        std::swap(w1ptr2[i],w2ptr2[i]);
		            }
		         }
                     }
	             
                     for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
				 ////GPU
				 cu_axpystream(size2, result0ptr[i], w1ptr2[i], -mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
			}
                     }
                    for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu]){   
			    cu_axpystream(size, resultptr[i], w1ptr[i], mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
                        }
                    }

                  }
            device_synchronize(GPU_streams,NUM_STREAMS);  
ENDt_TIMER("computation 1");
print("conds = ",conds," conds2 = "," FLOP = ",((long)conds)*320000 + ((long)conds2)*20000);
	            
print("conds2 = ",conds2," FLOP = ",((long)conds2)*20000);
            device_synchronize(GPU_streams,NUM_STREAMS);  
            
            CPUtransfer_buffer(rptr_arrayCPU, rptr_arrayGPU, rptr_off);
            CPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_arrayGPU, r0ptr_off);

            for (i = 0; i < inArgs.size(); i++){
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	            Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
                    R* resultptr = &rptr_arrayCPU[rptr_offarray[i]];
                    R* result0ptr = &r0ptr_arrayCPU[r0ptr_offarray[i]];
                    memcpy(result.ptr(), resultptr, result.size()*sizeof(R));
                    memcpy(result0.ptr(), result0ptr, result0.size()*sizeof(R));
		    Tensor<R> * r1 = new Tensor<R>(r_array[i]); 
		    Tensor<R> * r01 = new Tensor<R>(r0_array[i]); 
		    std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, *coeffs_array[i], argsdest_array[i], argstol_array[i], argsfac_array[i]);
                    outArg.push_back(t2);
            }

            GPUdelete_buffer(w1_array); //GPU 
            GPUdelete_buffer(w2_array); //GPU
            GPUdelete_buffer(w1_array2); //GPU 
            GPUdelete_buffer(w2_array2); //GPU
            GPUdelete_buffer(w5_array); 

            delete[] rptr_arrayCPU; //CPU
            delete[] r0ptr_arrayCPU; //CPU
            delete[] f0ptr_arrayCPU; //CPU
            delete[] fptr_arrayCPU;  //CPU

            delete[] w1temp;
            delete[] w1temp2;
            delete[] w1ptr;
            delete[] w2ptr;
            delete[] w1ptr2;
            delete[] w2ptr2;
            delete[] fptr;
            delete[] f0ptr;
            delete[] resultptr;
            delete[] result0ptr;

            GPUdelete_buffer(rptr_arrayGPU); //GPU
            GPUdelete_buffer(r0ptr_arrayGPU); //GPU
            GPUdelete_buffer(f0ptr_arrayGPU); //GPU
            GPUdelete_buffer(fptr_arrayGPU);  //GPU

            delete[] args; 
            
            delete[] r_array;
            delete[] r0_array;
            delete[] argstol_array;
            delete[] argsfac_array;
            delete[] argsdest_array;
            delete[] coeffs_array;
            delete[] tol_array;
            delete[] f_array;
            delete[] f0_array;
           
            for (i = 0; i < inArgs.size(); i++){
              unsigned int j;
              for (j = 0; j < rank; j++){
                delete trans[i][j];
                delete trans2[i][j]; 
              }
              delete trans[i];
              delete trans2[i];
            }
            delete trans;
            delete trans2;

            delete transU_CPU;
            delete trans2U_CPU;
            delete transVT_CPU;
            delete trans2VT_CPU;

            for (i = 0; i < inArgs.size(); i++){
              delete doit2[i];
              delete doit1[i];
            }

            for (i = 0; i < inArgs.size(); i++){
              delete condition[i];
            } 
            delete condition;

            delete n_array;

            for (i = 0; i < inArgs.size(); i++){
              delete mufacs[i];
            } 
            delete mufacs;

            return outArg;
        }

        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allComputeGPUIndKernels_Cublas5(std::vector<std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > > inArgs, 
                      std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print("      apply_allComputeGPU              ",inArgs.size());
            print("-----------BATCH-----------------");
            
           typedef TENSOR_RESULT_TYPE(T,Q) resultT;
	   typedef resultT R;
	   typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

            unsigned int i;

            STARTt_TIMER;
            //outside S
            //condition, doit2, doit1, trans, trans2, v2kref, vkref, f, f0 
            //coeffs, argsdest, argstol, argsfac, n, w1,w2,w5_[off]

            unsigned int* w1_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w2_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w5_offarray = new unsigned int[inArgs.size()]; //not used, because no shrink

            bool** condition;

            condition = new bool*[inArgs.size()];
           
            for (i = 0; i < inArgs.size(); i++){
              condition[i] = new bool[rank];
            }

            //on CPU
            Tensor<R> * r_array = new Tensor<R>[inArgs.size()];
            Tensor<R> * r0_array = new Tensor<R>[inArgs.size()];
            double * argstol_array = new double[inArgs.size()]; 
            double * argsfac_array = new double[inArgs.size()]; 
            keyT * argsdest_array = new keyT[inArgs.size()];
            dcT ** coeffs_array = new dcT*[inArgs.size()]; 
            double * tol_array = new double[inArgs.size()]; 
            Tensor<T> * f_array = new Tensor<T>[inArgs.size()];
            Tensor<T> * f0_array = new Tensor<T>[inArgs.size()];           
 
            Transformation *** trans;
            Transformation *** trans2;
            trans = new Transformation**[inArgs.size()];
            trans2 = new Transformation**[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                  unsigned int j;
                  trans[i] = new Transformation*[rank];
                  trans2[i] = new Transformation*[rank];
                  for (j = 0; j < rank; j++){
                    trans[i][j] = new Transformation[NDIM];
                    trans2[i][j] = new Transformation[NDIM];
                  }
            }

	    const long twok = 2*k;
	    long break_even;
	   
            Q* transU_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2U_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
            Q* transVT_CPU = new Q[twok*twok * (NDIM*rank*inArgs.size())]; 
            Q* trans2VT_CPU = new Q[k*k * (NDIM*rank*inArgs.size())]; 
 
	    if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
	    long break_even2;
	    if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);
            
            Level * n_array = new Level[inArgs.size()];

            bool** doit2;
            bool** doit1;
            doit2 = new bool*[inArgs.size()];
            doit1 = new bool*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              doit2[i] = new bool[rank];
              doit1[i] = new bool[rank];
            }

            Q** mufacs = new Q*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              mufacs[i] = new Q[rank];
            }

            std::tr1::tuple<keyT, keyT, keyT,
                  double, double, double,
                  Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > ** args = new std::tr1::tuple<keyT, keyT, keyT,
                                                                                           double, double, double,
                                                                                           Tensor<TENSOR_RESULT_TYPE(T,Q)>,
                                                                                           WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > *[inArgs.size()];

            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg;
            ENDt_TIMER("alloc & init");

            //STARTt_TIMER; 
            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));

		  keyT& source = std::tr1::get<0>(*args[i]);
		  keyT& shift = std::tr1::get<1>(*args[i]);

	          argsdest_array[i] = std::tr1::get<2>(*args[i]);
	          argstol_array[i] = std::tr1::get<3>(*args[i]);
	          argsfac_array[i] = std::tr1::get<4>(*args[i]);
	          double argscnorm = std::tr1::get<5>(*args[i]);
	          Tensor<R>& coeff = std::tr1::get<6>(*args[i]);
	          coeffs_array[i] = &(std::tr1::get<7>(*args[i]));
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
	          const std::vector<Slice>& s0ref = inObj.at(i)->s0;

	          tol_array[i] = argstol_array[i]/argsfac_array[i]/argscnorm;

		  const Tensor<T>* input = &coeff;
		  Tensor<T> dummy;

		  if (coeff.dim(0) == k) {
			// This processes leaf nodes with only scaling
			// coefficients ... FuncImpl::apply by default does not
			// apply the operator to these since for smoothing operators
			// it is not necessary.  It is necessary for operators such
			// as differentiation and time evolution and will also occur
			// if the application of the operator widens the tree.
			dummy = Tensor<T>(v2kref);
			dummy(s0ref) = coeff;
			input = &dummy;
		  }
		  else {
	              MADNESS_ASSERT(coeff.dim(0)==2*k);
		  }

                  tol_array[i] = tol_array[i]/rank;
 
		  const Tensor<T> f0 = copy(coeff(s0ref));
                  f0_array[i] = f0;
		    
		  const Tensor<T>& f = *input;
                  f_array[i] = f;
	
                  print(i);
                  STARTt_TIMER;	
                  const SeparatedConvolutionData<Q,NDIM>* op = getopGPU(source.level(), shift);
                  ENDt_TIMER("op");
		  Level n = source.level();

                  n_array[i] = n;

                  for (int mu=0; mu<rank ; ++mu) {
		      const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
		      if (muop.norm > tol_array[i]) {
                        condition[i][mu] = true;
		        Q fac = inObj.at(i)->ops[mu].getfac(); //same for the same mu and SeparatedConvolution instance

		        //glue
		        const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
		        double tol1 = tol_array[i]/std::abs(fac);
		        const Q mufac = fac;
                        mufacs[i][mu] = fac;
		     
		        double Rnorm = 1.0;
		        for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		        if (Rnorm == 0.0){
                          condition[i][mu] = false;
                          continue;
                        }

		        tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		        // Determine rank of SVD to use or if to use the full matrix
		        for (std::size_t d=0; d<NDIM; ++d) {
			  long r1;
			  for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			  }
			  if (r1 >= break_even) {
			    trans[i][mu][d].r = twok;
			    
                            trans[i][mu][d].U = ops[d]->GPUR;
                            //MADNESS_ASSERT(trans[i][mu][d].U != 0);
			    
                            trans[i][mu][d].VT = 0;
			  }
			  else {
			    r1 += (r1&1L);
			    trans[i][mu][d].r = std::max(2L,r1);

			    trans[i][mu][d].U = ops[d]->GPURU;
                            //MADNESS_ASSERT(trans[i][mu][d].U != 0);

			    trans[i][mu][d].VT = ops[d]->GPURVT;
                            //MADNESS_ASSERT(trans[i][mu][d].VT != 0);
			  }
		        }
		        ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

                            
                        // If all blocks are full rank we can skip the transposes
		        doit2[i][mu] = false;
		        for (std::size_t d=0; d<NDIM; ++d) doit2[i][mu] = doit2[i][mu] || trans[i][mu][d].VT; //move this out of the loop, calculate it in previous one

                        //trans2
		        if (n > 0) {

                          for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1< k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans2[i][mu][d].r = k; 

				trans2[i][mu][d].U = ops[d]->GPUT;
                                //MADNESS_ASSERT(trans[i][mu][d].U != 0);

				trans2[i][mu][d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans2[i][mu][d].r = std::max(2L,r1);

				trans2[i][mu][d].U = ops[d]->GPUTU;
                                //MADNESS_ASSERT(trans[i][mu][d].U != 0);

				trans2[i][mu][d].VT = ops[d]->GPUTVT;
                                //MADNESS_ASSERT(trans[i][mu][d].VT != 0);
			    }
			  }
			  
                          // If all blocks are full rank we can skip the transposes
			  doit1[i][mu] = false;
			  for (std::size_t d=0; d<NDIM; ++d) doit1[i][mu] = doit1[i][mu] || trans2[i][mu][d].VT;
			  
			  ////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
			  //const Tensor<T>& f1 = f0;
                        }
                      }
                      else condition[i][mu] = false;
                    }
       
            }
            //outside E
            //ENDt_TIMER("parallelizable");

            STARTt_TIMER;
            print("k = ",k);
            print("rank = ",rank);

            //twok = 2*k;
	   
            long dim2k, dimk;
	    dim2k = twok;
            dimk = k;

            long size = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size *= dim2k;
            long dimi = size/dim2k;

            long size2 = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size2 *= dimk;
            long dimi2 = size2/dimk;

 
            R* rptr_arrayCPU; //transfer to GPU
            R* r0ptr_arrayCPU; //transfer to GPU
            T* f0ptr_arrayCPU; //transfer to GPU
            T* fptr_arrayCPU;  //transfer to GPU
            R* rptr_arrayGPU; //transfer CPU <-> GPU
            R* r0ptr_arrayGPU; //transfer CPU <-> GPU
            T* f0ptr_arrayGPU; //transfer to GPU
            T* fptr_arrayGPU;  //transfer to GPU
            
            unsigned int rptr_off = 0;
            unsigned int r0ptr_off = 0;
            unsigned int f0ptr_off = 0;
            unsigned int fptr_off = 0;
 
            unsigned int* rptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* r0ptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* f0ptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU
            unsigned int* fptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU

            unsigned int w1_off = 0;
            unsigned int w2_off = 0;
            unsigned int w5_off = 0;

            for (i = 0; i < inArgs.size(); i++){
                  T* f0ptr = const_cast<T*>(f0_array[i].ptr());
                  T* fptr = const_cast<T*>(f_array[i].ptr());
           
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
			    
		  Tensor<resultT> r(v2kref);
		  Tensor<resultT> r0(vkref);
                  r_array[i] = r;
                  r0_array[i] = r0;

                  rptr_offarray[i] = rptr_off;
                  r0ptr_offarray[i] = r0ptr_off;
                  f0ptr_offarray[i] = f0ptr_off;
                  fptr_offarray[i] = fptr_off;

                  rptr_off += r_array[i].size();
                  r0ptr_off += r0_array[i].size();
                  f0ptr_off += f0_array[i].size();
                  fptr_off += f_array[i].size();
		  
                  Tensor<resultT> work1(v2kref,false), work2(v2kref,false);
		  Tensor<Q> work5(2*k, 2*k);
 
                 w1_offarray[i] = w1_off;
                  w2_offarray[i] = w2_off;
                  w5_offarray[i] = w5_off;
                  
                  w1_off += work1.size();
                  w2_off += work2.size();
                  w5_off += work5.size();
            } 

            fptr_arrayCPU = new T[fptr_off];
            rptr_arrayCPU = new R[rptr_off];
            r0ptr_arrayCPU = new R[r0ptr_off];
            f0ptr_arrayCPU = new T[f0ptr_off];
            
            for (i = 0; i < inArgs.size(); i++){ 
                  memcpy(rptr_arrayCPU + rptr_offarray[i], r_array[i].ptr(), r_array[i].size()*sizeof(R));
                  memcpy(r0ptr_arrayCPU + r0ptr_offarray[i], r0_array[i].ptr(), r0_array[i].size()*sizeof(R));
                  memcpy(f0ptr_arrayCPU + f0ptr_offarray[i], f0_array[i].ptr(), f0_array[i].size()*sizeof(T));
                  memcpy(fptr_arrayCPU + fptr_offarray[i], f_array[i].ptr(), f_array[i].size()*sizeof(T));
            }

            rptr_arrayGPU = GPUtransfer_buffer(rptr_arrayCPU, rptr_off, true); //both on CPU and GPU
            r0ptr_arrayGPU = GPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_off, true); //both on CPU and GPU
            f0ptr_arrayGPU = GPUtransfer_buffer(f0ptr_arrayCPU, f0ptr_off, true); //both on CPU and GPU
            fptr_arrayGPU = GPUtransfer_buffer(fptr_arrayCPU, fptr_off, true); //both on CPU and GPU
            
            R* w1_array = 0; //allocate on GPU
            R* w2_array = 0; //allocate GPU
            R* w1_array2 = 0; //allocate on GPU
            R* w2_array2 = 0; //allocate GPU
            Q* w5_array = 0; //do not use, because no shrink

            w1_array = GPUtransfer_buffer(w1_array, w1_off, false);
            w2_array = GPUtransfer_buffer(w2_array, w2_off, false);
            w1_array2 = GPUtransfer_buffer(w1_array2, w1_off, false);
            w2_array2 = GPUtransfer_buffer(w2_array2, w2_off, false);
            w5_array = GPUtransfer_buffer(w5_array, w5_off, false);

            R** w1ptr = new R*[inArgs.size()];
	    R** w2ptr = new R*[inArgs.size()];
            R** w1ptr2 = new R*[inArgs.size()];
	    R** w2ptr2 = new R*[inArgs.size()];
            T** f0ptr = new T*[inArgs.size()];
	    T** fptr = new T*[inArgs.size()];
            R** resultptr = new R*[inArgs.size()];
            R** result0ptr = new R*[inArgs.size()];
           
	    for (i = 0; i < inArgs.size(); i++){			    
	      //if (condition[i][mu]) {

		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		    w1ptr[i] = w1_array + w1_offarray[i];
		    w2ptr[i] = w2_array + w2_offarray[i];
		    w1ptr2[i] = w1_array2 + w1_offarray[i];
		    w2ptr2[i] = w2_array2 + w2_offarray[i];
		    f0ptr[i] = f0ptr_arrayGPU + f0ptr_offarray[i];
		    fptr[i] = fptr_arrayGPU + fptr_offarray[i];
		    resultptr[i] = rptr_arrayGPU + rptr_offarray[i];
		    result0ptr[i] = r0ptr_arrayGPU + r0ptr_offarray[i];
              //}
            }
            
			    
            const Q* U;

            pthread_mutex_lock(&apply_lock);	
	    if (apply_prev_offset < apply_curr_offset){
	      GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, apply_buffer + apply_prev_offset, 
				        apply_curr_offset - apply_prev_offset);
	      apply_prev_offset = apply_curr_offset;
	    }
            pthread_mutex_unlock(&apply_lock);
            ENDt_TIMER("transfer");	   

            int conds = 0; 
            int conds2 = 0;
conds = 0;
STARTt_TIMER;
            GPU_streams=streams_initialize(NUM_STREAMS, cublas_handle); 
            for (int mu=0; mu<rank; ++mu) {

	        for (i = 0; i < inArgs.size(); i++){			    
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                            U = trans[i][mu][0].U;

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                 }
	        
	            for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
                            conds2++;
                            U = trans2[i][mu][0].U;
		            ////GPU
                            
		            cu_mTxmqnewstream(dimi2, dimk, dimk, w1ptr2[i], f0ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                        }
                    }
	            
                 for (std::size_t d=1; d<NDIM; ++d) {
		      for (i = 0; i < inArgs.size(); i++){			    
			if (condition[i][mu]) {
                          conds++;
                          U = trans[i][mu][d].U;
			  ////GPU
			  cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			  ////GPU
			  std::swap(w1ptr[i],w2ptr[i]);
			}
		      }
		  }
		  
                    for (std::size_t d=1; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && n_array[i] > 0){
                                    conds2++;
                                    U = trans2[i][mu][d].U;
				    ////GPU
				    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
				    ////GPU
                                    std::swap(w1ptr2[i],w2ptr2[i]);
			    }
                        }
                    }
	            
		  for (std::size_t d=0; d<NDIM; ++d) {
	            for (i = 0; i < inArgs.size(); i++){			    
			if (doit2[i][mu] & condition[i][mu]) {
			    if (trans[i][mu][d].VT) {
                                conds++;
                                U = trans[i][mu][d].VT;
				////GPU
				cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    }
			    else {
				////GPU
				fast_transpose(dim2k, dimi, w1ptr[i], w2ptr[i]);
			    }
			    ////GPU
			    std::swap(w1ptr[i],w2ptr[i]);
			}
                      }
		    }
                    
                    for (std::size_t d=0; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && doit1[i][mu] && n_array[i] > 0) {
					if (trans2[i][mu][d].VT) {
                                            U = trans2[i][mu][d].VT;
                                            conds2++;
					    ////GPU
					    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
					}
					else {
					    ////GPU
					    fast_transpose(dimk, dimi2, w1ptr2[i], w2ptr2[i]);
					}
					////GPU
                                        std::swap(w1ptr2[i],w2ptr2[i]);
		            }
		         }
                     }
	             
                     for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
				 ////GPU
				 cu_axpystream(size2, result0ptr[i], w1ptr2[i], -mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
			}
                     }
                    for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu]){   
			    cu_axpystream(size, resultptr[i], w1ptr[i], mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
                        }
                    }

                  }
            device_synchronize(GPU_streams,NUM_STREAMS);  
ENDt_TIMER("computation 1");
print("conds = ",conds," conds2 = "," FLOP = ",((long)conds)*320000 + ((long)conds2)*20000);
	            
print("conds2 = ",conds2," FLOP = ",((long)conds2)*20000);
            device_synchronize(GPU_streams,NUM_STREAMS);  
            
            CPUtransfer_buffer(rptr_arrayCPU, rptr_arrayGPU, rptr_off);
            CPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_arrayGPU, r0ptr_off);

            for (i = 0; i < inArgs.size(); i++){
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	            Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
                    R* resultptr = &rptr_arrayCPU[rptr_offarray[i]];
                    R* result0ptr = &r0ptr_arrayCPU[r0ptr_offarray[i]];
                    memcpy(result.ptr(), resultptr, result.size()*sizeof(R));
                    memcpy(result0.ptr(), result0ptr, result0.size()*sizeof(R));
		    Tensor<R> * r1 = new Tensor<R>(r_array[i]); 
		    Tensor<R> * r01 = new Tensor<R>(r0_array[i]); 
		    std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, *coeffs_array[i], argsdest_array[i], argstol_array[i], argsfac_array[i]);
                    outArg.push_back(t2);
            }

            GPUdelete_buffer(w1_array); //GPU 
            GPUdelete_buffer(w2_array); //GPU
            GPUdelete_buffer(w1_array2); //GPU 
            GPUdelete_buffer(w2_array2); //GPU
            GPUdelete_buffer(w5_array); 

            delete[] rptr_arrayCPU; //CPU
            delete[] r0ptr_arrayCPU; //CPU
            delete[] f0ptr_arrayCPU; //CPU
            delete[] fptr_arrayCPU;  //CPU

            delete[] w1ptr;
            delete[] w2ptr;
            delete[] w1ptr2;
            delete[] w2ptr2;
            delete[] fptr;
            delete[] f0ptr;
            delete[] resultptr;
            delete[] result0ptr;

            GPUdelete_buffer(rptr_arrayGPU); //GPU
            GPUdelete_buffer(r0ptr_arrayGPU); //GPU
            GPUdelete_buffer(f0ptr_arrayGPU); //GPU
            GPUdelete_buffer(fptr_arrayGPU);  //GPU

            delete[] args; 
            
            delete[] r_array;
            delete[] r0_array;
            delete[] argstol_array;
            delete[] argsfac_array;
            delete[] argsdest_array;
            delete[] coeffs_array;
            delete[] tol_array;
            delete[] f_array;
            delete[] f0_array;
           
            for (i = 0; i < inArgs.size(); i++){
              unsigned int j;
              for (j = 0; j < rank; j++){
                delete trans[i][j];
                delete trans2[i][j]; 
              }
              delete trans[i];
              delete trans2[i];
            }
            delete trans;
            delete trans2;

            delete transU_CPU;
            delete trans2U_CPU;
            delete transVT_CPU;
            delete trans2VT_CPU;

            for (i = 0; i < inArgs.size(); i++){
              delete doit2[i];
              delete doit1[i];
            }

            for (i = 0; i < inArgs.size(); i++){
              delete condition[i];
            } 
            delete condition;

            delete n_array;

            for (i = 0; i < inArgs.size(); i++){
              delete mufacs[i];
            } 
            delete mufacs;

            return outArg;
        }

        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allComputeGPUIndKernels_Cublas6(std::vector<std::tr1::tuple<bool*, Transformation**, Transformation**,
                                                          bool*, bool*, Q*, Level, keyT, double, double,
                                                          WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                                                          Tensor<T>, Tensor<T> > > inArgs, 
                                              std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print("      apply_allComputeGPU              ",inArgs.size());
            print("-----------BATCH-----------------");
            
           typedef TENSOR_RESULT_TYPE(T,Q) resultT;
	   typedef resultT R;
	   typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

            unsigned int i;

            std::tr1::tuple<bool*, Transformation**, Transformation**,
                  bool*, bool*, Q*, Level, keyT, double, double, 
                  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                  Tensor<T>, Tensor<T> > ** args = new std::tr1::tuple<bool*, Transformation**, Transformation**,
                                                                       bool*, bool*, Q*, Level, keyT, double, double, 
                                                                       WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                                                                       Tensor<T>, Tensor<T> > *[inArgs.size()];

            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));
            }

            STARTt_TIMER;
            //outside S
            //condition, doit2, doit1, trans, trans2, v2kref, vkref, f, f0 
            //coeffs, argsdest, argstol, argsfac, n, w1,w2,w5_[off]

            unsigned int* w1_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w2_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w5_offarray = new unsigned int[inArgs.size()]; //not used, because no shrink

            bool** condition;

            condition = new bool*[inArgs.size()];
           
            for (i = 0; i < inArgs.size(); i++){
              condition[i] = std::tr1::get<0>(*args[i]);;
            }
 
            Transformation *** trans;
            Transformation *** trans2;
            trans = new Transformation**[inArgs.size()];
            trans2 = new Transformation**[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                trans[i] = std::tr1::get<1>(*args[i]);
                trans2[i] = std::tr1::get<2>(*args[i]);
            }

            bool** doit2;
            bool** doit1;
            doit2 = new bool*[inArgs.size()];
            doit1 = new bool*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                doit2[i] = std::tr1::get<3>(*args[i]);
                doit1[i] = std::tr1::get<4>(*args[i]);
            }

            Q** mufacs = new Q*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              mufacs[i] = std::tr1::get<5>(*args[i]);
            }

            

            //on CPU
            Tensor<R> * r_array = new Tensor<R>[inArgs.size()];
            Tensor<R> * r0_array = new Tensor<R>[inArgs.size()];
            double * argstol_array = new double[inArgs.size()]; 
            double * argsfac_array = new double[inArgs.size()]; 
            keyT * argsdest_array = new keyT[inArgs.size()];
            dcT ** coeffs_array = new dcT*[inArgs.size()]; 
            Tensor<T> * f_array = new Tensor<T>[inArgs.size()];
            Tensor<T> * f0_array = new Tensor<T>[inArgs.size()];           

            Level * n_array = new Level[inArgs.size()];

            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg;
            ENDt_TIMER("alloc & init");

            //STARTt_TIMER; 
            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));

                  n_array[i] = std::tr1::get<6>(*args[i]);
	          argsdest_array[i] = std::tr1::get<7>(*args[i]);
	          argstol_array[i] = std::tr1::get<8>(*args[i]);
	          argsfac_array[i] = std::tr1::get<9>(*args[i]);
	          coeffs_array[i] = &(std::tr1::get<10>(*args[i]));
                  f_array[i] = std::tr1::get<11>(*args[i]);
                  f0_array[i] = std::tr1::get<12>(*args[i]);
            }
            //outside E
            //ENDt_TIMER("parallelizable");

            STARTt_TIMER;
            print("k = ",k);
            print("rank = ",rank);

            //twok = 2*k;
	   
            long dim2k, dimk;
	    dim2k = 2*k;
            dimk = k;

            long size = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size *= dim2k;
            long dimi = size/dim2k;

            long size2 = 1;
            for (std::size_t ii=0; ii<NDIM; ++ii) size2 *= dimk;
            long dimi2 = size2/dimk;

 
            R* rptr_arrayCPU; //transfer to GPU
            R* r0ptr_arrayCPU; //transfer to GPU
            T* f0ptr_arrayCPU; //transfer to GPU
            T* fptr_arrayCPU;  //transfer to GPU
            R* rptr_arrayGPU; //transfer CPU <-> GPU
            R* r0ptr_arrayGPU; //transfer CPU <-> GPU
            T* f0ptr_arrayGPU; //transfer to GPU
            T* fptr_arrayGPU;  //transfer to GPU
            
            unsigned int rptr_off = 0;
            unsigned int r0ptr_off = 0;
            unsigned int f0ptr_off = 0;
            unsigned int fptr_off = 0;
 
            unsigned int* rptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* r0ptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* f0ptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU
            unsigned int* fptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU

            unsigned int w1_off = 0;
            unsigned int w2_off = 0;
            unsigned int w5_off = 0;

            for (i = 0; i < inArgs.size(); i++){
                  T* f0ptr = const_cast<T*>(f0_array[i].ptr());
                  T* fptr = const_cast<T*>(f_array[i].ptr());
                  //print(f0_array[i], f0_array[i].ptr());          
                  //print(f_array[i], f_array[i].ptr());          
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
			    
		  Tensor<resultT> r(v2kref);
		  Tensor<resultT> r0(vkref);
                  r_array[i] = r;
                  r0_array[i] = r0;

                  rptr_offarray[i] = rptr_off;
                  r0ptr_offarray[i] = r0ptr_off;
                  f0ptr_offarray[i] = f0ptr_off;
                  fptr_offarray[i] = fptr_off;
                  
                  //print(rptr_offarray[i], r0ptr_offarray[i], f0ptr_offarray[i], fptr_offarray[i]);

                  rptr_off += r_array[i].size();
                  r0ptr_off += r0_array[i].size();
                  f0ptr_off += f0_array[i].size();
                  fptr_off += f_array[i].size();
	
                  //print(rptr_off, r0ptr_off, f0ptr_off, fptr_off);
                  //print(rptr_offarray[i], r0ptr_offarray[i], f0ptr_offarray[i], fptr_offarray[i]);
	   
                  Tensor<resultT> work1(v2kref,false), work2(v2kref,false);
                  Tensor<Q> work5(2*k, 2*k);

                 w1_offarray[i] = w1_off;
                  w2_offarray[i] = w2_off;
                  w5_offarray[i] = w5_off;
                  
                  w1_off += work1.size();
                  w2_off += work2.size();
                  w5_off += work5.size();
                  //print(w1_off, w2_off, w5_off);
            } 

            fptr_arrayCPU = new T[fptr_off];
            rptr_arrayCPU = new R[rptr_off];
            r0ptr_arrayCPU = new R[r0ptr_off];
            f0ptr_arrayCPU = new T[f0ptr_off];
            
            for (i = 0; i < inArgs.size(); i++){ 
                  memcpy(rptr_arrayCPU + rptr_offarray[i], r_array[i].ptr(), r_array[i].size()*sizeof(R));
                  memcpy(r0ptr_arrayCPU + r0ptr_offarray[i], r0_array[i].ptr(), r0_array[i].size()*sizeof(R));
                  memcpy(f0ptr_arrayCPU + f0ptr_offarray[i], f0_array[i].ptr(), f0_array[i].size()*sizeof(T));
                  memcpy(fptr_arrayCPU + fptr_offarray[i], f_array[i].ptr(), f_array[i].size()*sizeof(T));
            }

            rptr_arrayGPU = GPUtransfer_buffer(rptr_arrayCPU, rptr_off, true); //both on CPU and GPU
            r0ptr_arrayGPU = GPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_off, true); //both on CPU and GPU
            f0ptr_arrayGPU = GPUtransfer_buffer(f0ptr_arrayCPU, f0ptr_off, true); //both on CPU and GPU
            fptr_arrayGPU = GPUtransfer_buffer(fptr_arrayCPU, fptr_off, true); //both on CPU and GPU
            
            R* w1_array = 0; //allocate on GPU
            R* w2_array = 0; //allocate GPU
            R* w1_array2 = 0; //allocate on GPU
            R* w2_array2 = 0; //allocate GPU
            Q* w5_array = 0; //do not use, because no shrink

            w1_array = GPUtransfer_buffer(w1_array, w1_off, false);
            w2_array = GPUtransfer_buffer(w2_array, w2_off, false);
            w1_array2 = GPUtransfer_buffer(w1_array2, w1_off, false);
            w2_array2 = GPUtransfer_buffer(w2_array2, w2_off, false);
            w5_array = GPUtransfer_buffer(w5_array, w5_off, false);

            R** w1ptr = new R*[inArgs.size()];
	    R** w2ptr = new R*[inArgs.size()];
            R** w1ptr2 = new R*[inArgs.size()];
	    R** w2ptr2 = new R*[inArgs.size()];
            T** f0ptr = new T*[inArgs.size()];
	    T** fptr = new T*[inArgs.size()];
            R** resultptr = new R*[inArgs.size()];
            R** result0ptr = new R*[inArgs.size()];
           
	    for (i = 0; i < inArgs.size(); i++){			    
	      //if (condition[i][mu]) {

		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		    w1ptr[i] = w1_array + w1_offarray[i];
		    w2ptr[i] = w2_array + w2_offarray[i];
		    w1ptr2[i] = w1_array2 + w1_offarray[i];
		    w2ptr2[i] = w2_array2 + w2_offarray[i];
		    f0ptr[i] = f0ptr_arrayGPU + f0ptr_offarray[i];
		    fptr[i] = fptr_arrayGPU + fptr_offarray[i];
		    resultptr[i] = rptr_arrayGPU + rptr_offarray[i];
		    result0ptr[i] = r0ptr_arrayGPU + r0ptr_offarray[i];
              //}
            }
            
			    
            const Q* U;
	
            pthread_mutex_lock(&apply_lock);	
	    if (apply_prev_offset < apply_curr_offset){
	      GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, apply_buffer + apply_prev_offset, 
				        apply_curr_offset - apply_prev_offset);
	      apply_prev_offset = apply_curr_offset;
	    }
            pthread_mutex_unlock(&apply_lock);	
            ENDt_TIMER("transfer");	   

            int conds = 0; 
            int conds2 = 0;
conds = 0;
STARTt_TIMER;
            GPU_streams=streams_initialize(NUM_STREAMS, cublas_handle); 
            for (int mu=0; mu<rank; ++mu) {

	        for (i = 0; i < inArgs.size(); i++){			    
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                            U = trans[i][mu][0].U;

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                 }
	        
	            for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
                            conds2++;
                            U = trans2[i][mu][0].U;
		            ////GPU
                            
		            cu_mTxmqnewstream(dimi2, dimk, dimk, w1ptr2[i], f0ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                        }
                    }
	            
                 for (std::size_t d=1; d<NDIM; ++d) {
		      for (i = 0; i < inArgs.size(); i++){			    
			if (condition[i][mu]) {
                          conds++;
                          U = trans[i][mu][d].U;
			  ////GPU
			  cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			  ////GPU
			  std::swap(w1ptr[i],w2ptr[i]);
			}
		      }
		  }
		  
                    for (std::size_t d=1; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && n_array[i] > 0){
                                    conds2++;
                                    U = trans2[i][mu][d].U;
				    ////GPU
				    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
				    ////GPU
                                    std::swap(w1ptr2[i],w2ptr2[i]);
			    }
                        }
                    }
	            
		  for (std::size_t d=0; d<NDIM; ++d) {
	            for (i = 0; i < inArgs.size(); i++){			    
			if (doit2[i][mu] & condition[i][mu]) {
			    if (trans[i][mu][d].VT) {
                                conds++;
                                U = trans[i][mu][d].VT;
				////GPU
				cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    }
			    else {
				////GPU
				fast_transpose(dim2k, dimi, w1ptr[i], w2ptr[i]);
			    }
			    ////GPU
			    std::swap(w1ptr[i],w2ptr[i]);
			}
                      }
		    }
                    
                    for (std::size_t d=0; d<NDIM; ++d) {
	                for (i = 0; i < inArgs.size(); i++){			 
                            if (condition[i][mu] && doit1[i][mu] && n_array[i] > 0) {
					if (trans2[i][mu][d].VT) {
                                            U = trans2[i][mu][d].VT;
                                            conds2++;
					    ////GPU
					    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
					}
					else {
					    ////GPU
					    fast_transpose(dimk, dimi2, w1ptr2[i], w2ptr2[i]);
					}
					////GPU
                                        std::swap(w1ptr2[i],w2ptr2[i]);
		            }
		         }
                     }
	             
                     for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu] && n_array[i] > 0){
				 ////GPU
				 cu_axpystream(size2, result0ptr[i], w1ptr2[i], -mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
			}
                     }
                    for (i = 0; i < inArgs.size(); i++){			 
                        if (condition[i][mu]){   
			    cu_axpystream(size, resultptr[i], w1ptr[i], mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
                        }
                    }

                  }
            device_synchronize(GPU_streams,NUM_STREAMS);  
ENDt_TIMER("computation 1");
print("conds = ",conds," conds2 = "," FLOP = ",((long)conds)*320000 + ((long)conds2)*20000);
	            
print("conds2 = ",conds2," FLOP = ",((long)conds2)*20000);
            device_synchronize(GPU_streams,NUM_STREAMS);  
            
            CPUtransfer_buffer(rptr_arrayCPU, rptr_arrayGPU, rptr_off);
            CPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_arrayGPU, r0ptr_off);

            for (i = 0; i < inArgs.size(); i++){
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	            Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
                    R* resultptr = &rptr_arrayCPU[rptr_offarray[i]];
                    R* result0ptr = &r0ptr_arrayCPU[r0ptr_offarray[i]];
                    memcpy(result.ptr(), resultptr, result.size()*sizeof(R));
                    memcpy(result0.ptr(), result0ptr, result0.size()*sizeof(R));
		    Tensor<R> * r1 = new Tensor<R>(r_array[i]); 
		    Tensor<R> * r01 = new Tensor<R>(r0_array[i]); 
		    std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, *coeffs_array[i], argsdest_array[i], argstol_array[i], argsfac_array[i]);
                    outArg.push_back(t2);
            }

            GPUdelete_buffer(w1_array); //GPU 
            GPUdelete_buffer(w2_array); //GPU
            GPUdelete_buffer(w1_array2); //GPU 
            GPUdelete_buffer(w2_array2); //GPU
            GPUdelete_buffer(w5_array); 

            delete[] rptr_arrayCPU; //CPU
            delete[] r0ptr_arrayCPU; //CPU
            delete[] f0ptr_arrayCPU; //CPU
            delete[] fptr_arrayCPU;  //CPU

            delete[] w1ptr;
            delete[] w2ptr;
            delete[] w1ptr2;
            delete[] w2ptr2;
            delete[] fptr;
            delete[] f0ptr;
            delete[] resultptr;
            delete[] result0ptr;

            GPUdelete_buffer(rptr_arrayGPU); //GPU
            GPUdelete_buffer(r0ptr_arrayGPU); //GPU
            GPUdelete_buffer(f0ptr_arrayGPU); //GPU
            GPUdelete_buffer(fptr_arrayGPU);  //GPU

            delete[] args; 
            
            delete[] r_array;
            delete[] r0_array;
            delete[] argstol_array;
            delete[] argsfac_array;
            delete[] argsdest_array;
            delete[] coeffs_array;
            delete[] f_array;
            delete[] f0_array;
           
            for (i = 0; i < inArgs.size(); i++){
              unsigned int j;
              for (j = 0; j < rank; j++){
                delete trans[i][j];
                delete trans2[i][j]; 
              }
              delete trans[i];
              delete trans2[i];
            }
            delete trans;
            delete trans2;

            for (i = 0; i < inArgs.size(); i++){
              delete doit2[i];
              delete doit1[i];
            }

            for (i = 0; i < inArgs.size(); i++){
              delete condition[i];
            } 
            delete condition;

            delete n_array;

            for (i = 0; i < inArgs.size(); i++){
              delete mufacs[i];
            } 
            delete mufacs;

            return outArg;
        }

        //Need a separate cache, which keeps 6 contiguous buffers of GPUR,GPURU,GPURVT,GPUT,GPUTU,GPUTV, and a bool for Rnormf
        //resulted from the same getop
        //Preprocess first checks this cache and, if the pair is in cache, sets trans/trans2 to the GPU values
        //obtained from that cache (setting them here is optional, since they'll be overwritten with the GPU
        //pointer anyways)
        //If the pair is not in cache, do getop, and, if they're not already there, put the elements in the 
        //original cache and optionally set trans, then, in the ever running task, for each i check 
        //the separate cache and, if still the pair is not in the separate cache, access op, set the 6
        //contiguous buffers and bool for Rnormf (use memcpy for the 6 contiguous buffers) and
        //transfer them to the GPU buffer, create the struct with 6 pointers and values of bool for Rnormf
        //and put it in separate cache   

        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allComputeGPU_OneKernel(std::vector<std::tr1::tuple<bool*, Transformation**, Transformation**,
                                                          bool*, bool*, Q*, Level, keyT, double, double,
                                                          WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                                                          Tensor<T>, Tensor<T>, SC*, keyT> > inArgs, 
                                              std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print("      apply_allComputeGPU              ",inArgs.size());
            print("-----------BATCH-----------------");
            
           typedef TENSOR_RESULT_TYPE(T,Q) resultT;
	   typedef resultT R;
	   typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;

            unsigned int i;

STARTt_TIMER;
            std::tr1::tuple<bool*, Transformation**, Transformation**,
                  bool*, bool*, Q*, Level, keyT, double, double, 
                  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                  Tensor<T>, Tensor<T>, SC*, keyT> ** args = new std::tr1::tuple<bool*, Transformation**, Transformation**,
                                                                       bool*, bool*, Q*, Level, keyT, double, double, 
                                                                       WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >,
                                                                       Tensor<T>, Tensor<T>, SC*, keyT> *[inArgs.size()];

            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));
            }

            //outside S
            //condition, doit2, doit1, trans, trans2, v2kref, vkref, f, f0 
            //coeffs, argsdest, argstol, argsfac, n, w1,w2,w5_[off]

            unsigned int* w1_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w2_offarray = new unsigned int[inArgs.size()]; //only on GPU
            unsigned int* w5_offarray = new unsigned int[inArgs.size()]; //not used, because no shrink

            bool** condition;

            condition = new bool*[inArgs.size()];
           
            for (i = 0; i < inArgs.size(); i++){
              condition[i] = std::tr1::get<0>(*args[i]);;
            }
 
            Transformation *** trans;
            Transformation *** trans2;
            trans = new Transformation**[inArgs.size()];
            trans2 = new Transformation**[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                trans[i] = std::tr1::get<1>(*args[i]);
                trans2[i] = std::tr1::get<2>(*args[i]);
            }

            bool** doit2;
            bool** doit1;
            doit2 = new bool*[inArgs.size()];
            doit1 = new bool*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
                doit2[i] = std::tr1::get<3>(*args[i]);
                doit1[i] = std::tr1::get<4>(*args[i]);
            }

            Q** mufacs = new Q*[inArgs.size()];
            for (i = 0; i < inArgs.size(); i++){
              mufacs[i] = std::tr1::get<5>(*args[i]);
            }

            

            //on CPU
            Tensor<R> * r_array = new Tensor<R>[inArgs.size()];
            Tensor<R> * r0_array = new Tensor<R>[inArgs.size()];
            double * argstol_array = new double[inArgs.size()]; 
            double * argsfac_array = new double[inArgs.size()]; 
            keyT * argsdest_array = new keyT[inArgs.size()];
            dcT ** coeffs_array = new dcT*[inArgs.size()]; 
            Tensor<T> * f_array = new Tensor<T>[inArgs.size()];
            Tensor<T> * f0_array = new Tensor<T>[inArgs.size()];           

            Level * n_array = new Level[inArgs.size()];

            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg;
            //ENDt_TIMER("alloc & init");

            SeparatedConvolutionData<Q, NDIM> ** op_data = new SeparatedConvolutionData<Q, NDIM> *[inArgs.size()];
            keyT* shifts = new keyT[inArgs.size()];
 
            //STARTt_TIMER; 
            for (i = 0; i < inArgs.size(); i++){
                  args[i] = &(inArgs.at(i));

                  n_array[i] = std::tr1::get<6>(*args[i]);
	          argsdest_array[i] = std::tr1::get<7>(*args[i]);
	          argstol_array[i] = std::tr1::get<8>(*args[i]);
	          argsfac_array[i] = std::tr1::get<9>(*args[i]);
	          coeffs_array[i] = &(std::tr1::get<10>(*args[i]));
                  f_array[i] = std::tr1::get<11>(*args[i]);
                  f0_array[i] = std::tr1::get<12>(*args[i]);
                  op_data[i] = (std::tr1::get<13>(*args[i]));
                  shifts[i] = std::tr1::get<14>(*args[i]); 
                  //print("shift[",i,"] =",shifts[i]); 
            }
            //outside E
            //ENDt_TIMER("parallelizable");

            //STARTt_TIMER;
     

 
            R* rptr_arrayCPU; //transfer to GPU
            R* r0ptr_arrayCPU; //transfer to GPU
            T* f0ptr_arrayCPU; //transfer to GPU
            T* fptr_arrayCPU;  //transfer to GPU
            R* rptr_arrayGPU; //transfer CPU <-> GPU
            R* r0ptr_arrayGPU; //transfer CPU <-> GPU
            T* f0ptr_arrayGPU; //transfer to GPU
            T* fptr_arrayGPU;  //transfer to GPU
            
            unsigned int rptr_off = 0;
            unsigned int r0ptr_off = 0;
            unsigned int f0ptr_off = 0;
            unsigned int fptr_off = 0;
 
            unsigned int* rptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* r0ptr_offarray = new unsigned int[inArgs.size()]; //only on GPU, result gets transfered to CPU
            unsigned int* f0ptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU
            unsigned int* fptr_offarray = new unsigned int[inArgs.size()]; //both on CPU and GPU

            unsigned int w1_off = 0;
            unsigned int w2_off = 0;
            unsigned int w5_off = 0;

            for (i = 0; i < inArgs.size(); i++){
                  T* f0ptr = const_cast<T*>(f0_array[i].ptr());
                  T* fptr = const_cast<T*>(f_array[i].ptr());
           
	          const std::vector<long>& vkref = inObj.at(i)->vk;
	          const std::vector<long>& v2kref = inObj.at(i)->v2k;
			    
		  Tensor<resultT> r(v2kref);
		  Tensor<resultT> r0(vkref);
                  r_array[i] = r;
                  r0_array[i] = r0;

                  rptr_offarray[i] = rptr_off;
                  r0ptr_offarray[i] = r0ptr_off;
                  f0ptr_offarray[i] = f0ptr_off;
                  fptr_offarray[i] = fptr_off;

                  rptr_off += r_array[i].size();
                  r0ptr_off += r0_array[i].size();
                  f0ptr_off += f0_array[i].size();
                  fptr_off += f_array[i].size();
		   
                  Tensor<resultT> work1(v2kref,false), work2(v2kref,false);
                  Tensor<Q> work5(2*k, 2*k);

                 w1_offarray[i] = w1_off;
                  w2_offarray[i] = w2_off;
                  w5_offarray[i] = w5_off;
                  
                  w1_off += work1.size();
                  w2_off += work2.size();
                  w5_off += work5.size();
            } 

            fptr_arrayCPU = new T[fptr_off + f0ptr_off];
            rptr_arrayCPU = new R[rptr_off + r0ptr_off];
            //r0ptr_arrayCPU = new R[r0ptr_off];
            r0ptr_arrayCPU = rptr_arrayCPU + rptr_off;
            //f0ptr_arrayCPU = new T[f0ptr_off];
            f0ptr_arrayCPU = fptr_arrayCPU + fptr_off;
            
            for (i = 0; i < inArgs.size(); i++){ 
                  memcpy(rptr_arrayCPU + rptr_offarray[i], r_array[i].ptr(), r_array[i].size()*sizeof(R));
                  memcpy(r0ptr_arrayCPU + r0ptr_offarray[i], r0_array[i].ptr(), r0_array[i].size()*sizeof(R));
                  memcpy(f0ptr_arrayCPU + f0ptr_offarray[i], f0_array[i].ptr(), f0_array[i].size()*sizeof(T));
                  memcpy(fptr_arrayCPU + fptr_offarray[i], f_array[i].ptr(), f_array[i].size()*sizeof(T));
            }
            //ENDt_TIMER("memcpy 1");

            //STARTt_TIMER;
            print("Copying input/output");
            rptr_arrayGPU = GPUtransfer_buffer(rptr_arrayCPU, rptr_off + r0ptr_off, true); //both on CPU and GPU
            //r0ptr_arrayGPU = GPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_off, true); //both on CPU and GPU
            r0ptr_arrayGPU = rptr_arrayGPU + rptr_off;
            //f0ptr_arrayGPU = GPUtransfer_buffer(f0ptr_arrayCPU, f0ptr_off, true); //both on CPU and GPU
            fptr_arrayGPU = GPUtransfer_buffer(fptr_arrayCPU, fptr_off + f0ptr_off, true); //both on CPU and GPU
            f0ptr_arrayGPU = fptr_arrayGPU + fptr_off;
            print("done.");
            //ENDt_TIMER("transfer 1");
            print("Transferred ",(rptr_off + r0ptr_off)*sizeof(R) + (fptr_off + f0ptr_off)*sizeof(T)," bytes.");
            
            R* w1_array = 0; //allocate on GPU
            R* w2_array = 0; //allocate GPU
            R* w1_array2 = 0; //allocate on GPU
            R* w2_array2 = 0; //allocate GPU
            Q* w5_array = 0; //do not use, because no shrink

            //STARTt_TIMER;
            print("Allocating data interm.");
            w1_array = GPUtransfer_buffer(w1_array, 2*(w1_off + w2_off), false);
            //w2_array = GPUtransfer_buffer(w2_array, w2_off, false);
            w2_array = w1_array + w1_off;
            //w1_array2 = GPUtransfer_buffer(w1_array2, w1_off, false);
            w1_array2 = w2_array + w2_off;
            //w2_array2 = GPUtransfer_buffer(w2_array2, w2_off, false);
            w2_array2 = w1_array2 + w1_off;
            w5_array = GPUtransfer_buffer(w5_array, w5_off, false);
            print("done.");
            //ENDt_TIMER("alloc 2");
            print(2*(w1_off + w2_off)*sizeof(R) + w5_off*sizeof(Q)," allocated."); 

            //STARTt_TIMER;
            R** w1ptr = new R*[inArgs.size()];
	    R** w2ptr = new R*[inArgs.size()];
            R** w1ptr2 = new R*[inArgs.size()];
	    R** w2ptr2 = new R*[inArgs.size()];
            T** f0ptr = new T*[inArgs.size()];
	    T** fptr = new T*[inArgs.size()];
            R** resultptr = new R*[inArgs.size()];
            R** result0ptr = new R*[inArgs.size()];
           
	    for (i = 0; i < inArgs.size(); i++){			    
	      //if (condition[i][mu]) {

		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
		     //Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
		    w1ptr[i] = w1_array + w1_offarray[i];
		    w2ptr[i] = w2_array + w2_offarray[i];
		    w1ptr2[i] = w1_array2 + w1_offarray[i];
		    w2ptr2[i] = w2_array2 + w2_offarray[i];
		    f0ptr[i] = f0ptr_arrayGPU + f0ptr_offarray[i];
		    fptr[i] = fptr_arrayGPU + fptr_offarray[i];
		    resultptr[i] = rptr_arrayGPU + rptr_offarray[i];
		    result0ptr[i] = r0ptr_arrayGPU + r0ptr_offarray[i];
              //}
            }
            //ENDt_TIMER("trans 1");

            //STARTt_TIMER;
            long ** rank_buf = new long*[inArgs.size()];            
            long ** rank2_buf = new long*[inArgs.size()];            
			    
            const Q* U;
            unsigned int twok = 2*k;
            unsigned int twoksq = 2*k*2*k;
            unsigned int ksq = k*k;
            unsigned int twokbytes = twok*twok*sizeof(Q);
            unsigned int kbytes = k*k*sizeof(Q);

            long dim2k, dimk;
	    dim2k = 2*k;
	    dimk = k;

	    long size = 1;
	    for (std::size_t ii=0; ii<NDIM; ++ii) size *= dim2k;
	    long dimi = size/dim2k;

	    long size2 = 1;
	    for (std::size_t ii=0; ii<NDIM; ++ii) size2 *= dimk;
	    long dimi2 = size2/dimk;

            unsigned int apply_init_offset = apply_prev_offset;

            GPUApplyBuffer<Q,NDIM> GPUab;
            unsigned int newSVD = 0;
            GPUApplyBuffer<Q,NDIM>* GPUab1;
            GPUApplyBuffer<Q,NDIM>** GPUab_array = new GPUApplyBuffer<Q,NDIM>*[inArgs.size()];
            unsigned int c = 0;
            for (i = 0; i < inArgs.size(); i++){
 

                const GPUApplyBuffer<Q,NDIM>* p = GPUApplyCache.getptr(n_array[i],shifts[i]);
                c = 0;
                
                for (int mu = 0; mu < rank; mu++){
                    if (condition[i][mu] || doit2[mu] || doit1[mu]){
                        c++;
                    }
                }
                //GPUab.svd_done = localSVD;
                
                if (!p){
                    unsigned int numR = rank*NDIM;
                    unsigned int numSVD = 0;
               
                     
                    GPUab.R = GPUapply_buffer + apply_prev_offset;
                    GPUab.T = GPUapply_buffer + apply_prev_offset + c*NDIM*twoksq;
                    GPUab.RU = GPUapply_buffer + apply_prev_offset + c*NDIM*(twoksq + ksq);
                    GPUab.TU = GPUapply_buffer + apply_prev_offset + c*NDIM*(2*twoksq + ksq);
                    GPUab.RVT = GPUapply_buffer + apply_prev_offset + c*NDIM*2*(twoksq + ksq);
                    GPUab.TVT = GPUapply_buffer + apply_prev_offset + c*NDIM*(3*twoksq + 2*ksq);

                    Q* localR = apply_buffer + apply_prev_offset - apply_init_offset; 
                    Q* localT = apply_buffer + apply_prev_offset - apply_init_offset + c*NDIM*twoksq;
                    Q* localRU = apply_buffer + apply_prev_offset - apply_init_offset + c*NDIM*(twoksq + ksq);
                    Q* localTU = apply_buffer + apply_prev_offset - apply_init_offset + c*NDIM*(2*twoksq + ksq);
                    Q* localRVT = apply_buffer + apply_prev_offset - apply_init_offset + c*NDIM*2*(twoksq + ksq);
                    Q* localTVT = apply_buffer + apply_prev_offset - apply_init_offset + c*NDIM*(3*twoksq + 2*ksq);
                    for (int mu = 0; mu < rank; mu++){
                        for (int d = 0; d < NDIM; d++){
                            if (condition[i][mu] || doit2[mu] || doit1[mu]){
                                memcpy(localR + numSVD*twoksq, op_data[i]->muops[mu].ops[d]->R.ptr(), twokbytes);
                                memcpy(localT + numSVD*ksq, op_data[i]->muops[mu].ops[d]->T.ptr(), kbytes);
                                memcpy(localRU + numSVD*twoksq, op_data[i]->muops[mu].ops[d]->RU.ptr(), twokbytes);
                                memcpy(localTU + numSVD*ksq, op_data[i]->muops[mu].ops[d]->TU.ptr(), kbytes);
                                memcpy(localRVT + numSVD*twoksq, op_data[i]->muops[mu].ops[d]->RVT.ptr(), twokbytes);
                                memcpy(localTVT + numSVD*ksq, op_data[i]->muops[mu].ops[d]->TVT.ptr(), kbytes);
                                numSVD++;
                            }
                        }
                    }
                    apply_prev_offset += c*NDIM*(3*twoksq + 3*ksq);


	            //if (apply_prev_offset < apply_curr_offset){
            
                    //print("Copying R and T(s)"); 
                    //if (c > 0){
	            //    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, localR, c*twokbytes);
	            //    apply_prev_offset += c*twoksq;
	            //    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, localT, c*kbytes);
	            //    apply_prev_offset += c*ksq;
                    //}
                    //if (numSVD > 0){
	            //    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, localRU, numSVD*twokbytes);
	            //    apply_prev_offset += numSVD*twoksq;
	            //    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, localTU, numSVD*kbytes);
	            //    apply_prev_offset += numSVD*ksq;
	            //    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, localRVT, numSVD*twokbytes);
	            //    apply_prev_offset += numSVD*twoksq;
	            //    GPUtransfer_buffernoalloc(GPUapply_buffer + apply_prev_offset, localTVT, numSVD*kbytes);
	            //    apply_prev_offset += numSVD*ksq;
                    //}
                    //print(c*twokbytes);
                    //print("done.");
                    //}

                     GPUApplyCache.set(n_array[i],shifts[i], GPUab);
                     GPUab_array[i] = GPUab1 = const_cast<GPUApplyBuffer<Q,NDIM>*>(GPUApplyCache.getptr(n_array[i],shifts[i]));
                     newSVD++;
                }
                else{
                    GPUab_array[i] = GPUab1 = const_cast<GPUApplyBuffer<Q,NDIM>*>(p);
                }

	        unsigned int temp = 0;
	        unsigned int temp2 = 0;
	        for (int mu = 0; mu < rank; mu++){
		    for (int d = 0; d < NDIM; d++){
			
                        memcpy(r_buffer + i*rank*NDIM + temp, &trans[i][mu][d].r, sizeof(long));
                        memcpy(r2_buffer + i*rank*NDIM + temp, &trans2[i][mu][d].r, sizeof(long));
                        //print("CPUr_buffer[",mu,d,"] = ",trans[i][mu][d].r);
                        //print("CPUr2_buffer[",mu,d,"] = ",trans2[i][mu][d].r);
                        if (condition[i][mu] || doit2[mu] || doit1[mu]){
		            if (trans[i][mu][d].VT == 0){
                                //MADNESS_ASSERT(trans[i][mu][d].r == twok);
			        trans[i][mu][d].U = GPUab1->R + temp*twoksq;
		            }
		            else{
                                //MADNESS_ASSERT(trans[i][mu][d].r < twok);
			        trans[i][mu][d].U = GPUab1->RU + temp*twoksq;  
			        trans[i][mu][d].VT = GPUab1->RVT + temp*twoksq;
		            }

		            if (trans2[i][mu][d].VT == 0){
                                //MADNESS_ASSERT(trans2[i][mu][d].r == k);
			        trans2[i][mu][d].U = GPUab1->T + temp*ksq;
		            }
		            else{
                                //MADNESS_ASSERT(trans2[i][mu][d].r < k);
			        trans2[i][mu][d].U = GPUab1->TU + temp*ksq;  
			        trans2[i][mu][d].VT = GPUab1->TVT + temp*ksq;
		            }

		            temp++;
                        }
		     }
	         }
 
                rank_buf[i] = GPUr_buffer + i*rank*NDIM;
 
                rank2_buf[i] = GPUr2_buffer + i*rank*NDIM;
            }

            print("Transferring ranks");
            GPUtransfer_buffernoalloc(GPUr_buffer, r_buffer, inArgs.size()*rank*NDIM);
	    GPUtransfer_buffernoalloc(GPUr2_buffer, r2_buffer, inArgs.size()*rank*NDIM);
            print("done.");

            //print(apply_prev_offset + newSVD*(rank*NDIM*(3*(ksq + twoksq))));
            //printf("%i \n",apply_buffer_maxsize);
                        MADNESS_ASSERT(apply_prev_offset  < apply_buffer_maxsize);
            GPUtransfer_buffernoalloc(GPUapply_buffer + apply_init_offset, apply_buffer, apply_prev_offset - apply_init_offset);

            MADNESS_ASSERT(sizeof(Q) == sizeof(R));
            bool* big_doit2;
            bool* big_doit1;
            Q* big_mufacs;
            alloc_host(&big_doit2, inArgs.size()*rank);
            alloc_host(&big_doit1, inArgs.size()*rank);
            alloc_host(&big_mufacs, inArgs.size()*rank);
            for (i = 0; i < inArgs.size(); i++){
                memcpy(big_doit2 + i*rank, doit2[i], rank*sizeof(bool));
                memcpy(big_doit1 + i*rank, doit1[i], rank*sizeof(bool));
                memcpy(big_mufacs + i*rank, mufacs[i], rank*sizeof(Q));
            }

            bool* doit1_GPU=GPUtransfer_buffer(big_doit1,inArgs.size()*rank,true);
            bool* doit2_GPU=GPUtransfer_buffer(big_doit2,inArgs.size()*rank,true);
            
	    Q* mufacs_GPU=GPUtransfer_buffer(big_mufacs,inArgs.size()*rank,true);

            cu_memset();//does cudaMemset of count
            //ENDt_TIMER("trans2");
            int conds = 0; 
            int conds2 = 0;
            GPU_streams=streams_initialize(NUM_STREAMS, cublas_handle); 
conds = 0;
//STARTt_TIMER;

if (NDIM == 3 && k == 11){
            for (i = 0; i < inArgs.size(); i++){
	
 
		    const Q *U,*RU,*VT;
		    Q *mufac_GPU;
		    bool *doitt_GPU;
		    U = GPUab_array[i]->R;
		    RU = GPUab_array[i]->RU;
		    VT= GPUab_array[i]->RVT;
		    mufac_GPU= mufacs_GPU+i*rank ;
		    doitt_GPU = doit2_GPU+i*rank ;
		    long * trans2r = rank_buf[i];
		    cu_mTxmq_integralop(dimi,dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U),GPU_streams[i%7],i%7,const_cast<Q*>(VT),doitt_GPU,mufac_GPU,resultptr[i], rank, trans2r, const_cast<Q*>(RU));
              }
         device_synchronize(GPU_streams,NUM_STREAMS);
//ENDt_TIMER("comp 1");

//STARTt_TIMER;
            for (i = 0; i < inArgs.size(); i++){
			    
			    long  dimk;
			    dimk = k;

                if (n_array[i] > 0){
			    int size = 1;
		            for (std::size_t ii=0; ii<NDIM; ++ii) size *= dimk;
		            long dimi = size/dimk;
			    
			    const Q *U,*VT,*TU;
			    Q *mufac_GPU;
			    bool *doitt_GPU;
			    U = GPUab_array[i]->T;
			    VT = GPUab_array[i]->TVT;
                            TU = GPUab_array[i]->TU;
			    mufac_GPU= mufacs_GPU+i*rank;
			    doitt_GPU = doit1_GPU+i*rank;
			    long * transr = rank2_buf[i];
                            ////GPU
			    cu_mTxmq_integralop(dimi,dimk, dimk, w1ptr2[i], f0ptr[i], const_cast<Q*>(U),GPU_streams[i%NUM_STREAMS],dimk,const_cast<Q*>(VT),doitt_GPU,mufac_GPU,result0ptr[i], rank, transr, const_cast<Q*>(TU));
		 }
               }
}
else{
               unsigned int * c_array = new unsigned int[inArgs.size()];
               for (i = 0; i < inArgs.size(); i++){
                     c_array[i] = 0;
               }
               for (int mu=0; mu<rank; ++mu) {

                     for (i = 0; i < inArgs.size(); i++){
	             if (condition[i][mu]) {
                           conds++;
		           //const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                            //U = trans[i][mu][0].U;
                            if (trans[i][mu][0].r == twok) U = GPUab_array[i]->R + c_array[i]*NDIM*twoksq;
                            else U = GPUab_array[i]->RU + c_array[i]*NDIM*twoksq;

                            ////GPU
			    cu_mTxmqnewstream(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    //cu_mTxmq_integralhundredOneWrite(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
			    //cu_mTxmq_integral4tb(dimi, dim2k, dim2k, w1ptr[i], fptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], dim2k);
                     }
                     }
	        
                     for (i = 0; i < inArgs.size(); i++){
                        if (condition[i][mu] && n_array[i] > 0){
                            if (trans2[i][mu][0].r == k) U = GPUab_array[i]->T + c_array[i]*NDIM*ksq;
                            else U = GPUab_array[i]->TU + c_array[i]*NDIM*ksq;
                            conds2++;
                            //U = trans2[i][mu][0].U;
		            ////GPU
                            
		            cu_mTxmqnewstream(dimi2, dimk, dimk, w1ptr2[i], f0ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
                        }
                      }
	            
                 for (i = 0; i < inArgs.size(); i++){
                 for (std::size_t d=1; d<NDIM; ++d) {
			if (condition[i][mu]) {
                          conds++;
                          //U = trans[i][mu][d].U;
                            if (trans[i][mu][d].r == twok) U = GPUab_array[i]->R + (c_array[i]*NDIM + d)*twoksq;
                            else U = GPUab_array[i]->RU + (c_array[i]*NDIM + d)*twoksq;
			  ////GPU
			  cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			  ////GPU
			  std::swap(w1ptr[i],w2ptr[i]);
			}
		  }
                  }
		  
                    for (i = 0; i < inArgs.size(); i++){
                    for (std::size_t d=1; d<NDIM; ++d) {
                            if (condition[i][mu] && n_array[i] > 0){
                                    conds2++;
                                    //U = trans2[i][mu][d].U;
                                   if (trans2[i][mu][d].r == k) U = GPUab_array[i]->T + (c_array[i]*NDIM + d)*ksq;
                                   else U = GPUab_array[i]->TU + (c_array[i]*NDIM + d)*ksq;
				    ////GPU
				    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
				    ////GPU
                                    std::swap(w1ptr2[i],w2ptr2[i]);
			    }
                    }
                    }
	            
                  for (i = 0; i < inArgs.size(); i++){
		  for (std::size_t d=0; d<NDIM; ++d) {
			if (doit2[i][mu] & condition[i][mu]) {
			    if (trans[i][mu][d].VT) {
                                conds++;
                                //U = trans[i][mu][d].VT;
                            if (trans[i][mu][d].r == twok) U = 0;
                            else U = GPUab_array[i]->RVT + (c_array[i]*NDIM + d)*twoksq;
                            MADNESS_ASSERT(U);
				////GPU
				cu_mTxmqnewstream(dimi, dim2k, dim2k, w2ptr[i], w1ptr[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
			    }
			    else {
				////GPU
				//fast_transpose(dim2k, dimi, w1ptr[i], w2ptr[i]);
                                //print("fast_cutrans");
				fast_cutrans(dim2k, dimi, w2ptr[i], w1ptr[i], GPU_streams[i%NUM_STREAMS]);
			    }
			    ////GPU
			    std::swap(w1ptr[i],w2ptr[i]);
			}
		    }
                    }
                    
                    for (i = 0; i < inArgs.size(); i++){
                    for (std::size_t d=0; d<NDIM; ++d) {
                            if (condition[i][mu] && doit1[i][mu] && n_array[i] > 0) {
					if (trans2[i][mu][d].VT) {
                                            //U = trans2[i][mu][d].VT;
                            if (trans2[i][mu][d].r == k) U = 0;
                            else U = GPUab_array[i]->TVT + (c_array[i]*NDIM + d)*ksq;
                            MADNESS_ASSERT(U);
                                            conds2++;
					    ////GPU
					    cu_mTxmqnewstream(dimi2, dimk, dimk, w2ptr2[i], w1ptr2[i], const_cast<Q*>(U), GPU_streams[i%NUM_STREAMS], 0, 0, cublas_handle);
					}
					else {
					    ////GPU
					    //fast_transpose(dimk, dimi2, w1ptr2[i], w2ptr2[i]);
                                            //print("fast_cutrans 10");
				            fast_cutrans(dimk, dimi2, w2ptr2[i], w1ptr2[i], GPU_streams[i%NUM_STREAMS]);
					}
					////GPU
                                        std::swap(w1ptr2[i],w2ptr2[i]);
		            }
                     }
                     }
	             
                        for (i = 0; i < inArgs.size(); i++){
                        if (condition[i][mu] && n_array[i] > 0){
				 ////GPU
				 cu_axpystream(size2, result0ptr[i], w1ptr2[i], -mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
			}
                        }
                        for (i = 0; i < inArgs.size(); i++){
                        if (condition[i][mu]){   
			    cu_axpystream(size, resultptr[i], w1ptr[i], mufacs[i][mu], GPU_streams[i%NUM_STREAMS], cublas_handle);
                        }
                        }
                 
                        for (i = 0; i < inArgs.size(); i++){
                            if (condition[i][mu] || doit2[mu] || doit1[mu]) c_array[i]++;
                        }
                  }
                  delete c_array;
}
           
		device_synchronize(GPU_streams,NUM_STREAMS);
//ENDt_TIMER("computation 2");
print("OP = ",(((unsigned long)conds)*2*2*k*2*k*2*k*2*k));          
  
            CPUtransfer_buffer(rptr_arrayCPU, rptr_arrayGPU, rptr_off);
            CPUtransfer_buffer(r0ptr_arrayCPU, r0ptr_arrayGPU, r0ptr_off);

            for (i = 0; i < inArgs.size(); i++){
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r_array[i];
	            Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0_array[i];
                    R* resultptr = &rptr_arrayCPU[rptr_offarray[i]];
                    R* result0ptr = &r0ptr_arrayCPU[r0ptr_offarray[i]];
                    memcpy(result.ptr(), resultptr, result.size()*sizeof(R));
                    memcpy(result0.ptr(), result0ptr, result0.size()*sizeof(R));
		    Tensor<R> * r1 = new Tensor<R>(r_array[i]); 
		    Tensor<R> * r01 = new Tensor<R>(r0_array[i]); 
		    std::tr1::tuple<Tensor<R>*, Tensor<R>*, dcT, keyT, double, double> t2(r1, r01, *coeffs_array[i], argsdest_array[i], argstol_array[i], argsfac_array[i]);
                    outArg.push_back(t2);
            }

            GPUdelete_buffer(w1_array); //GPU 
            //GPUdelete_buffer(w2_array); //GPU
            //GPUdelete_buffer(w1_array2); //GPU 
            //GPUdelete_buffer(w2_array2); //GPU
            GPUdelete_buffer(w5_array); 

            delete[] rptr_arrayCPU; //CPU
            //delete[] r0ptr_arrayCPU; //CPU
            //delete[] f0ptr_arrayCPU; //CPU
            delete[] fptr_arrayCPU;  //CPU

            delete[] w1ptr;
            delete[] w2ptr;
            delete[] w1ptr2;
            delete[] w2ptr2;
            delete[] fptr;
            delete[] f0ptr;
            delete[] resultptr;
            delete[] result0ptr;


            GPUdelete_buffer(rptr_arrayGPU); //GPU
            //GPUdelete_buffer(r0ptr_arrayGPU); //GPU
            //GPUdelete_buffer(f0ptr_arrayGPU); //GPU
            GPUdelete_buffer(fptr_arrayGPU);  //GPU

            delete[] args; 

            delete[] GPUab_array;

            delete[] rank_buf;           
            delete[] rank2_buf;           
            dealloc_host(big_doit2);
            dealloc_host(big_doit1);
            dealloc_host(big_mufacs);
            GPUdelete_buffer(doit2_GPU);
            GPUdelete_buffer(doit1_GPU);
            GPUdelete_buffer(mufacs_GPU);
 
            delete[] r_array;
            delete[] r0_array;
            delete[] argstol_array;
            delete[] argsfac_array;
            delete[] argsdest_array;
            delete[] coeffs_array;
            delete[] f_array;
            delete[] f0_array;
           
            for (i = 0; i < inArgs.size(); i++){
              unsigned int j;
              for (j = 0; j < rank; j++){
                delete trans[i][j];
                delete trans2[i][j]; 
              }
              delete trans[i];
              delete trans2[i];
            }
            delete[] trans;
            delete[] trans2;

            for (i = 0; i < inArgs.size(); i++){
              delete doit2[i];
              delete doit1[i];
            }
            delete[] doit2;
            delete[] doit1;

            for (i = 0; i < inArgs.size(); i++){
              delete condition[i];
            } 
            delete[] condition;

            delete[] n_array;

            for (i = 0; i < inArgs.size(); i++){
              delete mufacs[i];
            } 
            delete[] mufacs;

            delete[] op_data; delete[] shifts;
ENDt_TIMER("aggregation");
            return outArg;
        }
        #endif

        template <typename T, typename opT>
        std::vector<int>
        apply_allComputetry(std::vector<std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > > inArgs, 
                      std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print(inArgs.size());
            
            std::vector<int> outArg;
            
            for (unsigned int i = 0; i < inArgs.size(); i++){
                std::tr1::tuple<Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> temp = inObj.at(i)->apply_computept(inArgs.at(i));
                outArg.push_back(0);
                inObj.at(i)->apply_postprocesspt(temp);
                //if (i == 0) print("compute ",*(std::tr1::get<0>(temp)));
            }
 
            return outArg;
        }
       
        template <typename T, typename opT>
        std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> >
        apply_allCompute(std::vector<std::tr1::tuple<keyT, keyT, keyT, 
                                      double, double, double, 
                                      Tensor<TENSOR_RESULT_TYPE(T,Q)>, 
                                      WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > > > inArgs, 
                      std::vector< SeparatedConvolution<Q,NDIM>* > inObj) const {

            print(inArgs.size());
            
            std::vector< std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> > outArg/*(inArgs.size(), inObj.at(0)->apply_compute(inArgs.at(0)))*/;
            for (unsigned int i = 0; i < inArgs.size(); i++){
                std::tr1::tuple<Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *,
                         WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> temp = inObj.at(i)->apply_computept(inArgs.at(i));
                outArg.push_back(temp);
                // This was faster:
                //inObj.at(i)->apply_postprocesspt(temp);
                //if (i == 0) print("compute ",*(std::tr1::get<0>(temp)));
            }
 
            return outArg;
        }
        
        template <typename T>
        Void apply_postprocesspt(std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *, 
                               WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> t1) const{
            typedef Tensor<TENSOR_RESULT_TYPE(T,Q)> resultT;
            typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;
            resultT* r = std::tr1::get<0>(t1);
            resultT* r0 = std::tr1::get<1>(t1);
            dcT coeffs = std::tr1::get<2>(t1);
            keyT argsdest = std::tr1::get<3>(t1);
            double argstol = std::tr1::get<4>(t1);
            double argsfac = std::tr1::get<5>(t1);

            //print("process ",*r);
            //(*r)(s0).gaxpy(1.0,*r0,1.0);
            ////r->operator()(s0).gaxpy(1.0,*r0,1.0);
            ////resultT res = *r;
            resultT res = Tensor<TENSOR_RESULT_TYPE(T,Q)>(*r);
            res(s0).gaxpy(1.0,*r0,1.0);
            //print(*r);
            if (res./*r->*/normf()> 0.3*argstol/argsfac) {
                // OPTIMIZATION NEEDED HERE ... CHANGING THIS TO TASK NOT SEND REMOVED
                // BUILTIN OPTIMIZATION TO SHORTCIRCUIT MSG IF DATA IS LOCAL
                //print(*r);
                coeffs.task(argsdest, &FunctionNode<T,NDIM>::accumulate, res, coeffs, argsdest, TaskAttributes::hipri());
            }
            delete r0;
            delete r;
            return None;
        } 


        template <typename T>
        Void apply_postprocess(std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *, 
                               WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >&, keyT&, double&, double&> t1) const{
            typedef Tensor<TENSOR_RESULT_TYPE(T,Q)> resultT;
            typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;
            resultT* r = std::tr1::get<0>(t1);
            resultT* r0 = std::tr1::get<1>(t1);
            dcT& coeffs = std::tr1::get<2>(t1);
            const keyT& argsdest = std::tr1::get<3>(t1);
            const double& argstol = std::tr1::get<4>(t1);
            const double& argsfac = std::tr1::get<5>(t1);

            (*r)(s0).gaxpy(1.0,*r0,1.0);
            if (r->normf()> 0.3*argstol/argsfac) {
                // OPTIMIZATION NEEDED HERE ... CHANGING THIS TO TASK NOT SEND REMOVED
                // BUILTIN OPTIMIZATION TO SHORTCIRCUIT MSG IF DATA IS LOCAL
                coeffs.task(argsdest, &FunctionNode<T,NDIM>::accumulate, *r, coeffs, argsdest, TaskAttributes::hipri());
            }
            delete r0;
            delete r;
            return None;
        } 

        template <typename T>
        Void apply_postprocesscloser(std::tr1::tuple< Tensor<TENSOR_RESULT_TYPE(T,Q)> *, Tensor<TENSOR_RESULT_TYPE(T,Q)> *, 
                               WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> >, keyT, double, double> t1) const{
            typedef Tensor<TENSOR_RESULT_TYPE(T,Q)> resultT;
            typedef  WorldContainer<Key<NDIM> , FunctionNode<T, NDIM> > dcT;
            resultT* r = std::tr1::get<0>(t1);
            resultT* r0 = std::tr1::get<1>(t1);
            dcT& coeffs = std::tr1::get<2>(t1);
            const keyT argsdest = std::tr1::get<3>(t1);
            const double argstol = std::tr1::get<4>(t1);
            const double argsfac = std::tr1::get<5>(t1);

            r->operator()(s0).gaxpy(1.0,*r0,1.0);
            if (r->normf()> 0.3*argstol/argsfac) {
                // OPTIMIZATION NEEDED HERE ... CHANGING THIS TO TASK NOT SEND REMOVED
                // BUILTIN OPTIMIZATION TO SHORTCIRCUIT MSG IF DATA IS LOCAL
                coeffs.task(argsdest, &FunctionNode<T,NDIM>::accumulate, *r, coeffs, argsdest, TaskAttributes::hipri());
            }
            delete r0;
            delete r;
            return None;
        } 

        template <typename T>
        Tensor<TENSOR_RESULT_TYPE(T,Q)> opt_inlined_apply(const Key<NDIM>& source,
                                              const Key<NDIM>& shift,
                                              const Tensor<T>& coeff,
                                              double tol) const {
            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            print("inlined_apply \n");
            typedef TENSOR_RESULT_TYPE(T,Q) resultT;
            typedef resultT R;
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k), r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            const Tensor<T> f0 = copy(coeff(s0));
            
            Level n = source.level();
            const Tensor<T>& f = *input;
            Transformation trans[NDIM];
	    const long twok = 2*k;
	    long break_even;
	    
            if (NDIM==1) break_even = long(0.5*twok);
	    else if (NDIM==2) break_even = long(0.6*twok);
	    else if (NDIM==3) break_even=long(0.65*twok);
	    else break_even=long(0.7*twok);
	    
            long break_even2;
            if (NDIM==1) break_even2 = long(0.5*k);
	    else if (NDIM==2) break_even2 = long(0.6*k);
	    else if (NDIM==3) break_even2=long(0.65*k);
	    else break_even2=long(0.7*k);

            R* restrict w1=work1.ptr();
            R* restrict w2=work2.ptr();
	    Q* restrict w3=work5.ptr();

            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0) continue;

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[d].r = twok;
			    trans[d].U = ops[d]->R.ptr();
			    trans[d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[d].r = std::max(2L,r1);
			    trans[d].U = ops[d]->RU.ptr();
			    trans[d].VT = ops[d]->RVT.ptr();
			}
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		   
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                    ////GPU
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                        ////GPU
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
                        ////GPU
			std::swap(w1,w2);
		    }

		    // If all blocks are full rank we can skip the transposes
		    bool doit = false;
		    for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
                                ////GPU
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
                                ////GPU
				fast_transpose(dimk, dimi, w1, w2);
			    }
                            ////GPU
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
                    ////GPU
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);

		    if (n > 0) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even2) {
				trans[d].r = k;
				trans[d].U = ops[d]->T.ptr();
				trans[d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans[d].r = std::max(2L,r1);
				trans[d].U = ops[d]->TU.ptr();
				trans[d].VT = ops[d]->TVT.ptr();
			    }
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
                        ////GPU
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
                            ////GPU
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
                            ////GPU
		            std::swap(w1,w2);
			}

			// If all blocks are full rank we can skip the transposes
			bool doit = false;
			for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
                                    ////GPU
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				    size = dimk*size/trans[d].r;
				}
				else {
                                    ////GPU
			            fast_transpose(dimk, dimi, w1, w2);
				}
                                ////GPU
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
                         ////GPU
			 aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    }
                }
            }
            r(s0).gaxpy(1.0,r0,1.0);
            return r;
        }

        template <typename T>
        Tensor<TENSOR_RESULT_TYPE(T,Q)> inlined_apply(const Key<NDIM>& source,
                                              const Key<NDIM>& shift,
                                              const Tensor<T>& coeff,
                                              double tol) const {
            PROFILE_MEMBER_FUNC(SeparatedConvolution);
            print("inlined_apply \n");
            typedef TENSOR_RESULT_TYPE(T,Q) resultT;
            typedef resultT R;
            const Tensor<T>* input = &coeff;
            Tensor<T> dummy;

            if (coeff.dim(0) == k) {
                // This processes leaf nodes with only scaling
                // coefficients ... FuncImpl::apply by default does not
                // apply the operator to these since for smoothing operators
                // it is not necessary.  It is necessary for operators such
                // as differentiation and time evolution and will also occur
                // if the application of the operator widens the tree.
                dummy = Tensor<T>(v2k);
                dummy(s0) = coeff;
                input = &dummy;
            }
            else {
                MADNESS_ASSERT(coeff.dim(0)==2*k);
            }

            tol = tol/rank; // Error is per separated term

            const SeparatedConvolutionData<Q,NDIM>* op = getop(source.level(), shift);

            //print("sepop",source,shift,op->norm,tol);

            Tensor<resultT> r(v2k), r0(vk);
            Tensor<resultT> work1(v2k,false), work2(v2k,false);
            Tensor<Q> work5(2*k,2*k);


            const Tensor<T> f0 = copy(coeff(s0));
            for (int mu=0; mu<rank; ++mu) {
                const SeparatedConvolutionInternal<Q,NDIM>& muop =  op->muops[mu];
                //print("muop",source, shift, mu, muop.norm);
                if (muop.norm > tol) {
                    Q fac = ops[mu].getfac();
                    //muopxv_fast(source.level(), muop.ops, *input, f0, r, r0, tol/std::abs(fac), fac,
                    //            work1, work2, work5);

                    //glue
                    Level n = source.level();
                    const ConvolutionData1D<Q>* const* ops/*[NDIM]*/ = muop.ops;
                    const Tensor<T>& f = *input;
                    //const Tensor<T>& f0
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result = r;
                    Tensor<TENSOR_RESULT_TYPE(T,Q)>& result0 = r0;
                    double tol1 = tol/std::abs(fac);
                    const Q mufac = fac;
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work1
                    //Tensor<TENSOR_RESULT_TYPE(T,Q)>& work2
                    //Tensor<Q>& work5

                    Transformation trans[NDIM];
                     
		    double Rnorm = 1.0;
		    for (std::size_t d=0; d<NDIM; ++d) Rnorm *= ops[d]->Rnorm;
		    if (Rnorm == 0.0) continue;

		    tol1 = tol1/(Rnorm*NDIM);  // Errors are relative within here

		    // Determine rank of SVD to use or if to use the full matrix
		    const long twok = 2*k;
		    long break_even;
		    if (NDIM==1) break_even = long(0.5*twok);
		    else if (NDIM==2) break_even = long(0.6*twok);
		    else if (NDIM==3) break_even=long(0.65*twok);
		    else break_even=long(0.7*twok);
		    for (std::size_t d=0; d<NDIM; ++d) {
			long r1;
			for (r1=0; r1<twok; ++r1) {
			    if (ops[d]->Rs[r1] < tol1) break;
			}
			if (r1 >= break_even) {
			    trans[d].r = twok;
			    trans[d].U = ops[d]->R.ptr();
			    trans[d].VT = 0;
			}
			else {
			    r1 += (r1&1L);
			    trans[d].r = std::max(2L,r1);
			    trans[d].U = ops[d]->RU.ptr();
			    trans[d].VT = ops[d]->RVT.ptr();
			}
		    }
		    ////apply_transformation(n, twok, trans, f, work1, work2, work5, mufac, result);

		    long dimk = twok;
		    Tensor<Q>& work3 = work5;
		    
                    long size = 1;
		    for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
		    long dimi = size/dimk;

		    R* restrict w1=work1.ptr();
		    R* restrict w2=work2.ptr();
		    Q* restrict w3=work3.ptr();

		    const Q* U;

		    U = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
		    mTxmq(dimi, trans[0].r, dimk, w1, f.ptr(), U);
		    size = trans[0].r * size / dimk;
		    dimi = size/dimk;
		    for (std::size_t d=1; d<NDIM; ++d) {
			U = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
			mTxmq(dimi, trans[d].r, dimk, w2, w1, U);
			size = trans[d].r * size / dimk;
			dimi = size/dimk;
			std::swap(w1,w2);
		    }

		    // If all blocks are full rank we can skip the transposes
		    bool doit = false;
		    for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

		    if (doit) {
			for (std::size_t d=0; d<NDIM; ++d) {
			    if (trans[d].VT) {
				dimi = size/trans[d].r;
				mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				size = dimk*size/trans[d].r;
			    }
			    else {
				fast_transpose(dimk, dimi, w1, w2);
			    }
			    std::swap(w1,w2);
			}
		    }
		    // Assuming here that result is contiguous and aligned
		    aligned_axpy(size, result.ptr(), w1, mufac);
		    //    long one = 1;
		    //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);

		    if (n > 0) {
			if (NDIM==1) break_even = long(0.5*k);
			else if (NDIM==2) break_even = long(0.6*k);
			else if (NDIM==3) break_even=long(0.65*k);
			else break_even=long(0.7*k);
			for (std::size_t d=0; d<NDIM; ++d) {
			    long r1;
			    for (r1=0; r1<k; ++r1) {
				if (ops[d]->Ts[r1] < tol1) break;
			    }
			    if (r1 >= break_even) {
				trans[d].r = k;
				trans[d].U = ops[d]->T.ptr();
				trans[d].VT = 0;
			    }
			    else {
				r1 += (r1&1L);
				trans[d].r = std::max(2L,r1);
				trans[d].U = ops[d]->TU.ptr();
				trans[d].VT = ops[d]->TVT.ptr();
			    }
			}
			////apply_transformation(n, k, trans, f0, work1, work2, work5, -mufac, result0);
		        dimk = k;
                        const Tensor<T>& f1 = f0;
			Tensor<Q>& work31 = work5;
                        const Q mufac1 = -mufac;
                        Tensor<R>& result1 = result0;

			size = 1;
			for (std::size_t i=0; i<NDIM; ++i) size *= dimk;
			dimi = size/dimk;

			w1=work1.ptr();
			w2=work2.ptr();
			w3=work31.ptr();

			const Q* U1;

			U1 = (trans[0].r == dimk) ? trans[0].U : shrink(dimk,dimk,trans[0].r,trans[0].U,w3);
			mTxmq(dimi, trans[0].r, dimk, w1, f1.ptr(), U1);
			size = trans[0].r * size / dimk;
			dimi = size/dimk;
			for (std::size_t d=1; d<NDIM; ++d) {
	                    U1 = (trans[d].r == dimk) ? trans[d].U : shrink(dimk,dimk,trans[d].r,trans[d].U,w3);
			    mTxmq(dimi, trans[d].r, dimk, w2, w1, U1);
			    size = trans[d].r * size / dimk;
			    dimi = size/dimk;
		            std::swap(w1,w2);
			}

			// If all blocks are full rank we can skip the transposes
			bool doit = false;
			for (std::size_t d=0; d<NDIM; ++d) doit = doit || trans[d].VT;

			if (doit) {
			    for (std::size_t d=0; d<NDIM; ++d) {
				if (trans[d].VT) {
			            dimi = size/trans[d].r;
				    mTxmq(dimi, dimk, trans[d].r, w2, w1, trans[d].VT);
				    size = dimk*size/trans[d].r;
				}
				else {
			            fast_transpose(dimk, dimi, w1, w2);
				}
				std::swap(w1,w2);
		            }
			 }
			 // Assuming here that result is contiguous and aligned
			 aligned_axpy(size, result1.ptr(), w1, mufac1);
			 //    long one = 1;
			 //daxpy_(&size, &mufac, w1, &one, result.ptr(), &one);
                    }
                }
            }
            r(s0).gaxpy(1.0,r0,1.0);
            return r;
        }

    }; 

    /// Factory function generating separated kernel for convolution with 1/r in 3D.
    static
    inline
    SeparatedConvolution<double_complex,3> PeriodicHFExchangeOperator(World& world,
                                                   Vector<double,3> args,
                                                   double lo,
                                                   double eps,
                                                   const BoundaryConditions<3>& bc=FunctionDefaults<3>::get_bc(),
                                                   int k=FunctionDefaults<3>::get_k())
    {
        const Tensor<double>& cell_width = FunctionDefaults<3>::get_cell_width();
        double hi = cell_width.normf(); // Diagonal width of cell
        if (bc(0,0) == BC_PERIODIC) hi *= 100; // Extend range for periodic summation
        const double pi = constants::pi;

        // bsh_fit generates representation for 1/4Pir but we want 1/r
        // so have to scale eps by 1/4Pi

        Tensor<double> coeff, expnt;
        bsh_fit(0.0, lo, hi, eps/(4.0*pi), &coeff, &expnt, false);

        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, cell_width.max(), true);
        }
        coeff.scale(4.0*pi);
        return SeparatedConvolution<double_complex,3>(world, args, coeff, expnt, bc, k, false);
//        return SeparatedConvolution<double_complex,3>(world, coeff, expnt, bc, k);

    }

    /// Factory function generating separated kernel for convolution with 1/r in 3D.
    static
    inline
    SeparatedConvolution<double,3> CoulombOperator(World& world,
                                                   double lo,
                                                   double eps,
                                                   const BoundaryConditions<3>& bc=FunctionDefaults<3>::get_bc(),
                                                   int k=FunctionDefaults<3>::get_k())
    {
        const Tensor<double>& cell_width = FunctionDefaults<3>::get_cell_width();
        double hi = cell_width.normf(); // Diagonal width of cell
        if (bc(0,0) == BC_PERIODIC) hi *= 100; // Extend range for periodic summation
        const double pi = constants::pi;

        // bsh_fit generates representation for 1/4Pir but we want 1/r
        // so have to scale eps by 1/4Pi

        Tensor<double> coeff, expnt;
        bsh_fit(0.0, lo, hi, eps/(4.0*pi), &coeff, &expnt, false);

        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, cell_width.max(), true);
        }
        coeff.scale(4.0*pi);
        return SeparatedConvolution<double,3>(world, coeff, expnt, bc, k);
    }


    /// Factory function generating separated kernel for convolution with 1/r in 3D.
    static
    inline
    SeparatedConvolution<double,3>* CoulombOperatorPtr(World& world,
                                                       double lo,
                                                       double eps,
                                                       const BoundaryConditions<3>& bc=FunctionDefaults<3>::get_bc(),
                                                       int k=FunctionDefaults<3>::get_k())
    {
        const Tensor<double>& cell_width = FunctionDefaults<3>::get_cell_width();
        double hi = cell_width.normf(); // Diagonal width of cell
        if (bc(0,0) == BC_PERIODIC) hi *= 100; // Extend range for periodic summation
        const double pi = constants::pi;

        // bsh_fit generates representation for 1/4Pir but we want 1/r
        // so have to scale eps by 1/4Pi
        Tensor<double> coeff, expnt;
        bsh_fit(0.0, lo, hi, eps/(4.0*pi), &coeff, &expnt, false);
        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, cell_width.max(), true);
        }
        coeff.scale(4.0*pi);
        return new SeparatedConvolution<double,3>(world, coeff, expnt, bc, k);
    }


    /// Factory function generating separated kernel for convolution with BSH kernel in general NDIM
    template <std::size_t NDIM>
    static
    inline
    SeparatedConvolution<double,NDIM> BSHOperator(World& world,
                                                  double mu,
                                                  double lo,
                                                  double eps,
                                                  const BoundaryConditions<NDIM>& bc=FunctionDefaults<NDIM>::get_bc(),
                                                  int k=FunctionDefaults<NDIM>::get_k())
    {
        const Tensor<double>& cell_width = FunctionDefaults<NDIM>::get_cell_width();
        double hi = cell_width.normf(); // Diagonal width of cell
        if (bc(0,0) == BC_PERIODIC) hi *= 100; // Extend range for periodic summation
        Tensor<double> coeff, expnt;
        bsh_fit_ndim(NDIM, mu, lo, hi, eps, &coeff, &expnt, false);
        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, cell_width.max(), false);
        }

        return SeparatedConvolution<double,NDIM>(world, coeff, expnt, bc, k);
    }


    /// Factory function generating separated kernel for convolution with exp(-mu*r)/(4*pi*r) in 3D
    static
    inline
    SeparatedConvolution<double,3> BSHOperator3D(World& world,
                                                 double mu,
                                                 double lo,
                                                 double eps,
                                                 const BoundaryConditions<3>& bc=FunctionDefaults<3>::get_bc(),
                                                 int k=FunctionDefaults<3>::get_k())

    {
        const Tensor<double>& cell_width = FunctionDefaults<3>::get_cell_width();
        double hi = cell_width.normf(); // Diagonal width of cell
        if (bc(0,0) == BC_PERIODIC) hi *= 100; // Extend range for periodic summation
        Tensor<double> coeff, expnt;
        bsh_fit(mu, lo, hi, eps, &coeff, &expnt, false);
        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, cell_width.max(), false);
        }
        return SeparatedConvolution<double,3>(world, coeff, expnt, bc, k);
    }

    /// Factory function generating separated kernel for convolution with exp(-mu*r)/(4*pi*r) in 3D
    static
    inline
    SeparatedConvolution<double,3>* BSHOperatorPtr3D(World& world,
                                                     double mu,
                                                     double lo,
                                                     double eps,
                                                     const BoundaryConditions<3>& bc=FunctionDefaults<3>::get_bc(),
                                                     int k=FunctionDefaults<3>::get_k())
    {
        const Tensor<double>& cell_width = FunctionDefaults<3>::get_cell_width();
        double hi = cell_width.normf(); // Diagonal width of cell
        if (bc(0,0) == BC_PERIODIC) hi *= 100; // Extend range for periodic summation
        Tensor<double> coeff, expnt;
        bsh_fit(mu, lo, hi, eps, &coeff, &expnt, false);
        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, cell_width.max(), false);
        }
        return new SeparatedConvolution<double,3>(world, coeff, expnt, bc, k);
    }


    /// Factory function generating operator for convolution with grad(1/r) in 3D
    
    /// Returns a 3-vector containing the convolution operator for the
    /// x, y, and z components of grad(1/r)
    static
    inline
    std::vector< std::shared_ptr< SeparatedConvolution<double,3> > >
    GradCoulombOperator(World& world,
                        double lo,
                        double eps,
                        const BoundaryConditions<3>& bc=FunctionDefaults<3>::get_bc(),
                        int k=FunctionDefaults<3>::get_k())
    {
        typedef SeparatedConvolution<double,3> real_convolution_3d;
        typedef std::shared_ptr<real_convolution_3d> real_convolution_3d_ptr;
        const double pi = constants::pi;
        const Tensor<double> width = FunctionDefaults<3>::get_cell_width();
        double hi = width.normf(); // Diagonal width of cell
        const bool isperiodicsum = (bc(0,0)==BC_PERIODIC);
        if (isperiodicsum) hi *= 100; // Extend range for periodic summation
        
        // bsh_fit generates representation for 1/4Pir but we want 1/r
        // so have to scale eps by 1/4Pi
        Tensor<double> coeff, expnt;
        bsh_fit(0.0, lo, hi, eps/(4.0*pi), &coeff, &expnt, false);
        
        if (bc(0,0) == BC_PERIODIC) {
            truncate_periodic_expansion(coeff, expnt, width.max(), true);
        }
        
        coeff.scale(4.0*pi);
        int rank = coeff.dim(0);
        
        std::vector<real_convolution_3d_ptr> gradG(3);
        
        for (int dir=0; dir<3; dir++) {
            std::vector< ConvolutionND<double,3> > ops(rank);
            for (int mu=0; mu<rank; mu++) {
                // We cache the normalized operator so the factor is the value we must multiply
                // by to recover the coeff we want.
                double c = std::pow(sqrt(expnt(mu)/pi),3); // Normalization coeff
                ops[mu].setfac(coeff(mu)/c/width[dir]);
                
                for (int d=0; d<3; d++) {
                    if (d != dir)
                        ops[mu].setop(d,GaussianConvolution1DCache<double>::get(k, expnt(mu)*width[d]*width[d], 0, isperiodicsum));
                }
                ops[mu].setop(dir,GaussianConvolution1DCache<double>::get(k, expnt(mu)*width[dir]*width[dir], 1, isperiodicsum));
            }
            gradG[dir] = real_convolution_3d_ptr(new SeparatedConvolution<double,3>(world, ops));
        }
        
        return gradG;
    }

    namespace archive {
        template <class Archive, class T, std::size_t NDIM>
        struct ArchiveLoadImpl<Archive,const SeparatedConvolution<T,NDIM>*> {
            static inline void load(const Archive& ar, const SeparatedConvolution<T,NDIM>*& ptr) {
                WorldObject< SeparatedConvolution<T,NDIM> >* p;
                ar & p;
                ptr = static_cast< const SeparatedConvolution<T,NDIM>* >(p);
            }
        };

        template <class Archive, class T, std::size_t NDIM>
        struct ArchiveStoreImpl<Archive,const SeparatedConvolution<T,NDIM>*> {
            static inline void store(const Archive& ar, const SeparatedConvolution<T,NDIM>*const& ptr) {
                ar & static_cast< const WorldObject< SeparatedConvolution<T,NDIM> >* >(ptr);
            }
        };
    }

}




#endif // MADNESS_MRA_OPERATOR_H__INCLUDED
