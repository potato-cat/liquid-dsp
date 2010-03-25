/*
 * Copyright (c) 2007, 2008, 2009, 2010 Joseph Gaeddert
 * Copyright (c) 2007, 2008, 2009, 2010 Virginia Polytechnic
 *                                      Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// fir (finite impulse response) filter design using Parks-McClellan
// algorithm
//
// Much of this program has been borrowed heavily from [McClellan:1973]
// and [Janovetz:1998] with the exception of the Lagrange polynomial
// interpolation formulas.
//
// References:
//  [Parks:1972] T. W. Parks and J. H. McClellan, "Chebyshev
//      Approximation for Nonrecursive Digital Filters with Linear
//      Phase," IEEE Transactions on Circuit Theory, vol. CT-19,
//      no. 2, March 1972.
//  [McClellan:1973] J. H. McClellan, T. W. Parks, L. R. Rabiner, "A
//      Computer Program for Designing Optimum FIR Linear Phase
//      Digital Filters," IEEE Transactions on Audio and
//      Electroacoustics, vol. AU-21, No. 6, December 1973.
//  [Rabiner:1975] L. R. Rabiner, J. H. McClellan, T. W. Parks, "FIR
//      Digital filter Design Techniques Using Weighted Chebyshev
//      Approximations," Proceedings of the IEEE, March 1975.
//  [Janovetz:1998] J. Janovetz, online: http://www.janovetz.com/jake/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "liquid.internal.h"

#define LIQUID_FIRDESPM_DEBUG   1

// structured data type
struct firdespm_s {
    // constants
    unsigned int h_len;         // filter length
    unsigned int s;             // odd/even filter length
    unsigned int n;             // filter semi-length
    unsigned int r;             // number of approximating functions
    unsigned int num_bands;     // number of discrete bands
    unsigned int grid_size;     // number of points on the grid
    unsigned int grid_density;  // density of the grid

    // band type (e.g. LIQUID_FIRDESPM_BANDPASS)
    liquid_firdespm_btype btype;

    // filter description parameters
    float * bands;              // bands array [size: 2*num_bands]
    float * des;                // desired response [size: num_bands]
    float * weights;            // weights [size: num_bands]
    
    // dense grid elements
    float * F;                  // frequencies, [0, 0.5]
    float * D;                  // desired response
    float * W;                  // weight
    float * E;                  // error

    float * x;                  // Chebyshev points : cos(2*pi*f)
    float * alpha;              // Lagrange interpolating polynomial
    float * c;                  // interpolants
    float rho;                  // extremal weighted error

    unsigned int * iext;        // indices of extrema
    unsigned int num_changes;   // number of changes in extrema

#if LIQUID_FIRDESPM_DEBUG
    FILE * fid;
#endif

};

// create filter design object
firdespm firdespm_create(unsigned int _h_len,
                         float * _bands,
                         float * _des,
                         float * _weights,
                         unsigned int _num_bands,
                         liquid_firdespm_btype _btype)
{
    // TODO : validate input

    // create object
    firdespm q = (firdespm) malloc(sizeof(struct firdespm_s));

    // compute number of extremal frequencies
    q->h_len = _h_len;              // filter length
    q->s     = q->h_len % 2;        // odd/even length
    q->n     = (q->h_len - q->s)/2; // filter semi-length
    q->r     = q->n + q->s;         // number of approximating functions

    // allocate memory for extremal frequency set, interpolating polynomial
    q->iext  = (unsigned int*) malloc((q->r+1)*sizeof(unsigned int));
    q->x     = (float*) malloc((q->r+1)*sizeof(float));
    q->alpha = (float*) malloc((q->r+1)*sizeof(float));
    q->c     = (float*) malloc((q->r+1)*sizeof(float));

    // allocate memory for arrays
    q->num_bands = _num_bands;
    q->bands    = (float*) malloc(2*q->num_bands*sizeof(float));
    q->des      = (float*) malloc(  q->num_bands*sizeof(float));
    q->weights  = (float*) malloc(  q->num_bands*sizeof(float));

    // copy input arrays
    memmove(q->bands,   _bands,   2*q->num_bands*sizeof(float));
    memmove(q->des,     _des,       q->num_bands*sizeof(float));
    memmove(q->weights, _weights,   q->num_bands*sizeof(float));

    // TODO : estimate grid size
    q->grid_density = 16;
    q->grid_size = 1024;

    // create the grid
    q->F = (float*) malloc(q->grid_size*sizeof(float));
    q->D = (float*) malloc(q->grid_size*sizeof(float));
    q->W = (float*) malloc(q->grid_size*sizeof(float));
    q->E = (float*) malloc(q->grid_size*sizeof(float));
    firdespm_init_grid(q);
    // TODO : fix grid, weights according to filter type

    // return object
    return q;
}

void firdespm_destroy(firdespm _q)
{
    // free dense grid elements
    free(_q->F);
    free(_q->D);
    free(_q->W);
    free(_q->E);

    // free band description elements
    free(_q->bands);
    free(_q->des);
    free(_q->weights);

    // free object
    free(_q);
}

void firdespm_print(firdespm _q)
{
    unsigned int i;
    printf("firdespm:\n");
    for (i=0; i<_q->num_bands; i++)
        printf("  [%12.8f %12.8f]\n", _q->bands[2*i+0], _q->bands[2*i+1]);
}

void firdespm_execute(firdespm _q, float * _h)
{
    unsigned int i;

    // initial guess of extremal frequencies evenly spaced on F
    for (i=0; i<=_q->r; i++) {
        _q->iext[i] = (i * (_q->grid_size-1)) / _q->r;
        //printf("iext(%3u) = %u\n", i, iext[i]);
    }

    // iterate over the Remez exchange algorithm
    unsigned int p;
    unsigned int max_iterations = 1;
    for (p=0; p<max_iterations; p++) {
        // compute interpolator
        firdespm_compute_interp(_q);

        // compute error
        firdespm_compute_error(_q);

#if LIQUID_FIRDESPM_DEBUG
        FILE * fid = fopen("error.dat","w");
        for (i=0; i<_q->grid_size; i++)
            fprintf(fid,"%16.8e;\n", _q->E[i]);
        fclose(fid);
#endif

        // search for new extremal frequencies
        firdespm_iext_search(_q);

        // TODO : check exit criteria
    }

    // re-compute interpolator one last time
    //firdespm_compute_interp(_q);

#if 0
    // evaluate Lagrange polynomial on evenly spaced points
    unsigned int b=r;
    float G[2*b-1];
    for (i=0; i<=b; i++) {
        float f = (float)(i) / (float)(2*b-1);
        float xf = cosf(2*M_PI*f);
        float cf = fpolyval_lagrange_barycentric(x,c,beta,xf,ne-1);
        G[i] = cf;
        G[2*b-i-1] = cf;
    }
    //for (i=0; i<2*b-1; i++)
    //    printf("G(%3u) = %12.4e;\n", i+1, G[i]);

    // compute inverse DFT
    float h[b];
    for (i=0; i<b; i++) {
        h[i] = 0.0f;
        unsigned int j;
        for (j=0; j<2*b-1; j++) {
            float f = (float)(i) / (float) (2*b-1);
            h[i] += G[j] * cosf(2*M_PI*f*j);
        }
        h[i] /= 2*b-1;
    }
    for (i=0; i<b; i++)
        printf("h(%3u) = %12.8f;\n", i+1, h[i]);

    // TODO : perform transformation here for different filter types
#endif
}


// 
// internal methods
//

// initialize the frequency grid on the disjoint bounded set
void firdespm_init_grid(firdespm _q)
{
    unsigned int i,j;

    // validate input

    // frequency step size
    float df = 0.5f/(_q->grid_density*_q->r);
    printf("df : %12.8f\n", df);

    // number of grid points counter
    unsigned int n = 0;

    // TODO : take care of special symmetry conditions here

    float f0, f1;
    for (i=0; i<_q->num_bands; i++) {
        // extract band edges
        f0 = _q->bands[2*i+0];
        f1 = _q->bands[2*i+1];

        // compute the number of gridpoints in this band
        unsigned int num_points = (unsigned int)( (f1-f0)/df + 0.5 );
        printf("band : [%12.8f %12.8f] %3u points\n",f0,f1,num_points);

        // ensure at least one point per band
        if (num_points < 1) num_points = 1;

        // add points to grid
        for (j=0; j<num_points; j++) {
            // add frequency points
            _q->F[n] = f0 + j*df;

            // compute desired response
            // TODO : use function pointer
            _q->D[n] = _q->des[i];

            // compute weight
            // TODO : use function pointer?
            _q->W[n] = _q->weights[i];

            n++;
        }
        // force endpoint to be upper edge of frequency band
        _q->F[n-1] = f1;   // according to Janovetz
    }
    _q->grid_size = n;

    // TODO : take care of special symmetry conditions here
}

// compute interpolating polynomial
void firdespm_compute_interp(firdespm _q)
{
    unsigned int i;

    // compute Chebyshev points on F[iext[]] : cos(2*pi*f)
    for (i=0; i<=_q->r; i++) {
        _q->x[i] = cosf(2*M_PI*_q->F[_q->iext[i]]);
        printf("x[%3u] = %12.8f\n", i, _q->x[i]);
    }
    printf("\n");

    // compute Lagrange interpolating polynomial
    fpolyfit_lagrange_barycentric(_q->x,_q->r+1,_q->alpha);
    for (i=0; i<=_q->r; i++)
        printf("a[%3u] = %12.8f\n", i, _q->alpha[i]);

    // compute rho
    float t0 = 0.0f;    // numerator
    float t1 = 0.0f;    // denominator
    float rho;
    for (i=0; i<_q->r+1; i++) {
        t0 += _q->alpha[i] * _q->D[_q->iext[i]];
        t1 += _q->alpha[i] / _q->W[_q->iext[i]] * (i % 2 ? -1.0f : 1.0f);
    }
    rho = t0/t1;
    printf("  rho   :   %12.4e\n", rho);
    printf("\n");

    // compute polynomial values (interpolants)
    for (i=0; i<=_q->r; i++) {
        _q->c[i] = _q->D[_q->iext[i]] - (i % 2 ? -1 : 1) * rho / _q->W[i];
        printf("c[%3u] = %16.8e\n", i, _q->c[i]);
    }

}

void firdespm_compute_error(firdespm _q)
{
    unsigned int i;

    float xf;
    float H;
    for (i=0; i<_q->grid_size; i++) {
        // compute actual response
        xf = cosf(2*M_PI*_q->F[i]);
        H = fpolyval_lagrange_barycentric(_q->x,_q->c,_q->alpha,xf,_q->r+1);

        // compute error
        _q->E[i] = _q->W[i] * (_q->D[i] - H);
    }
}

// search error curve for r+1 extremal indices
// TODO : return number of values which have changed (exit criteria)
void firdespm_iext_search(firdespm _q)
{
    unsigned int i;

    // found extremal frequency indices
    unsigned int found_iext[2*_q->r];
    unsigned int num_found=0;

    // check for extremum at f=0
    if ( fabsf(_q->E[0]) > fabsf(_q->E[1]) )
        found_iext[num_found++] = 0;

    // search inside grid
    for (i=1; i<_q->grid_size-1; i++) {
        if ( ((_q->E[i]>0.0) && (_q->E[i-1]<_q->E[i]) && (_q->E[i+1]<_q->E[i]) ) ||
             ((_q->E[i]<0.0) && (_q->E[i-1]>_q->E[i]) && (_q->E[i+1]>_q->E[i]) ) )
        {
            found_iext[num_found++] = i;
        }
    }

    // check for extremum at f=0.5
    if ( fabsf(_q->E[i]) > fabsf(_q->E[i-1]) )
        found_iext[num_found++] = i;

#if LIQUID_FIRDESPM_DEBUG
    for (i=0; i<num_found; i++)
        printf("found_iext[%3u] = %5u : %18.8e\n", i, found_iext[i], _q->E[found_iext[i]]);
    FILE * fid = fopen("iext.dat","w");
    for (i=0; i<num_found; i++)
        fprintf(fid,"%u;\n", found_iext[i]+1);
    fclose(fid);
#endif

    // search extrema and eliminate smallest
    unsigned int imin=0;    // index of found_iext where _E is a minimum extreme
    unsigned int sign=0;    // sign of error
    unsigned int num_extra = num_found - _q->r - 1; // number of extra extremal frequencies
    unsigned int alternating_sign;

    while (num_extra) {
        // evaluate sign of first extrema
        sign = _q->E[found_iext[0]] > 0.0;

        //
        imin = 0;
        alternating_sign = 1;
        for (i=1; i<num_found; i++) {
            // update new minimum error extreme
            if ( fabsf(_q->E[found_iext[i]]) < fabsf(_q->E[found_iext[imin]]) ) {
                imin = i;
                printf("new min at i=%u\n", imin);
            }
    
            if ( sign && _q->E[found_iext[i]] < 0.0 ) {
                sign = 0;
            } else if ( !sign && _q->E[found_iext[i]] > 0.0 ) {
                sign = 1;
            } else {
                // found two extrema with non-alternating sign; delete
                // the smaller of the two
                if ( fabsf(_q->E[found_iext[i]]) < fabsf(_q->E[found_iext[i-1]]) )
                    imin = i;
                else
                    imin = i-1;
                alternating_sign = 0;
                break;
            }
        }
        printf("  imin : %3u : %12.4e;\n", imin, _q->E[found_iext[imin]]);

        // 
        if ( alternating_sign && num_extra==1) {
            //imin = (fabsf(_q->E[found_iext[0]]) > fabsf(_q->E[found_iext[num_extra-1]])) ? 0 : num_extra-1;
            if (fabsf(_q->E[found_iext[0]]) < fabsf(_q->E[found_iext[num_found-1]]))
                imin = 0;
            else
                imin = num_found-1;
        }

        // Delete value in 'found_iext' at 'index imin'.  This
        // is equivalent to shifing all values left one position
        // starting at index imin+1
        printf("deleting found_iext[%3u] = %3u\n", imin, found_iext[imin]);
#if 0
        memmove( &found_iext[imin],
                 &found_iext[imin+1],
                 (num_found-imin)*sizeof(unsigned int));
#else
        // equivalent code:
        for (i=imin; i<num_found; i++)
            found_iext[i] = found_iext[i+1];
#endif

        num_extra--;
        num_found--;
    }

#if LIQUID_FIRDESPM_DEBUG
    for (i=0; i<num_found; i++)
        printf("found_iext_new[%3u] = %u\n", i, found_iext[i]);
    fid = fopen("iext_new.dat","w");
    for (i=0; i<_q->r+1; i++)
        fprintf(fid,"%u;\n", found_iext[i]+1);
    fclose(fid);
#endif

    // count number of changes
    unsigned int num_changes=0;
    for (i=0; i<_q->r+1; i++)
        num_changes += _q->iext[i] == found_iext[i] ? 0 : 1;
    printf("changes : %u\n", num_changes);

    // copy new values
    memmove(_q->iext, found_iext, (_q->r+1)*sizeof(unsigned int));
}


