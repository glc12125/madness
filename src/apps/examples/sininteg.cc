#define WORLD_INSTANTIATE_STATIC_TEMPLATES  

/*!
  \file examples/sininteg.cc
  \brief Compute the integral sin(x) x=0..10
  \defgroup sininteg First example from getting started guide
  \ingroup examples

  Computes the integral 
  \f[
     \int_0^{10} sin(x) dx
  \f]
  by projecting \f$ sin(x) \f$ into the discontinuous spectral element
  basis and using the \c trace() method.

 */


#include <mra/mra.h>

using namespace madness;

double myf(const coord_1d& r) {
    return std::sin(r[0]);
}

int main(int argc, char** argv) {
    initialize(argc, argv);
    World world(MPI::COMM_WORLD);

    startup(world,argc,argv);

    FunctionDefaults<1>::set_cubic_cell(0,10);

    real_function_1d f = real_factory_1d(world).f(myf);

    double integral = f.trace();

    if (world.rank() == 0) print("The result is", integral);

    finalize();
    return 0;
}
