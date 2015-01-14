/*
**                 TNLMeans for VapourSynth
**
**   TNLMeans is an implementation of the NL-means denoising algorithm.
**   Aside from the original method, TNLMeans also supports extension
**   into 3D, a faster, block based approach, and a multiscale version.
**
**   Copyright (C) 2006-2007 Kevin Stone
**
**   This program is free software; you can redistribute it and/or modify
**   it under the terms of the GNU General Public License as published by
**   the Free Software Foundation; either version 2 of the License, or
**   (at your option) any later version.
**
**   This program is distributed in the hope that it will be useful,
**   but WITHOUT ANY WARRANTY; without even the implied warranty of
**   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**   GNU General Public License for more details.
**
**   You should have received a copy of the GNU General Public License
**   along with this program; if not, write to the Free Software
**   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "VapourSynth.h"
#include "AlignedMemory.h"
#include "PlanarFrame.h"
#include "TNLMeans.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

TNLMeans::TNLMeans
(
    int _Ax, int _Ay, int _Az,
    int _Sx, int _Sy,
    int _Bx, int _By,
    double _a, double _h, bool _ssd,
    const VSMap *in,
    VSMap       *out,
    const VSAPI *vsapi
) : Ax( _Ax ), Ay( _Ay ), Az( _Az ),
    Sx( _Sx ), Sy( _Sy ),
    Bx( _Bx ), By( _By ),
    a( _a ), h( _h ), ssd( _ssd )
{
    node =  vsapi->propGetNode( in, "clip", 0, 0 );
    vi   = *vsapi->getVideoInfo( node );
    fc = nullptr;
    dstPF = srcPFr = nullptr;
    gw = nullptr;
    weightsb = sumsb = nullptr;
    ds = nullptr;
    if( vi.format->colorFamily == cmCompat ) { vsapi->setError( out, "TNLMeans:  only planar formats are supported!"); return; }
    if( vi.format->bitsPerSample != 8 )      { vsapi->setError( out, "TNLMeans:  only 8-bit formats are supported!"); return; }
    if( h <= 0.0 ) { vsapi->setError( out, "TNLMeans:  h must be greater than 0!" );               return; }
    if( a <= 0.0 ) { vsapi->setError( out, "TNLMeans:  a must be greater than 0!" );               return; }
    if( Ax < 0 )   { vsapi->setError( out, "TNLMeans:  ax must be greater than or equal to 0!" );  return; }
    if( Ay < 0 )   { vsapi->setError( out, "TNLMeans:  ay must be greater than or equal to 0!" );  return; }
    if( Az < 0 )   { vsapi->setError( out, "TNLMeans:  az must be greater than or equal to 0!" );  return; }
    if( Bx < 0 )   { vsapi->setError( out, "TNLMeans:  bx must be greater than or equal to 0!" );  return; }
    if( By < 0 )   { vsapi->setError( out, "TNLMeans:  by must be greater than or equal to 0!" );  return; }
    if( Sx < 0 )   { vsapi->setError( out, "TNLMeans:  sx must be greater than or equal to 0!" );  return; }
    if( Sy < 0 )   { vsapi->setError( out, "TNLMeans:  sy must be greater than or equal to 0!" );  return; }
    if( Sx < Bx )  { vsapi->setError( out, "TNLMeans:  sx must be greater than or equal to bx!" ); return; }
    if( Sy < By )  { vsapi->setError( out, "TNLMeans:  sy must be greater than or equal to by!" ); return; }
    h2in = -1.0 / (h * h);
    hin = -1.0 / h;
    Sxd = Sx * 2 + 1;
    Syd = Sy * 2 + 1;
    Sxa = Sxd * Syd;
    Bxd = Bx * 2 + 1;
    Byd = By * 2 + 1;
    Bxa = Bxd * Byd;
    Axd = Ax * 2 + 1;
    Ayd = Ay * 2 + 1;
    Axa = Axd * Ayd;
    Azdm1 = Az * 2;
    a2 = a * a;
    dstPF = new PlanarFrame( vi );

    if( Az ) fc = new nlCache( Az * 2 + 1, (Bx > 0 || By > 0), vi );
    else srcPFr = new PlanarFrame( vi );

    if( Bx || By )
    {
        sumsb = static_cast<double *>(AlignedMemory::alloc( Bxa * sizeof(double), 16 ));
        if( !sumsb )
        {
            vsapi->setError( out, "TNLMeans:  malloc failure (sumsb)!" );
            return;
        }
        weightsb = static_cast<double *>(AlignedMemory::alloc( Bxa * sizeof(double), 16 ));
        if( !weightsb )
        {
            vsapi->setError( out, "TNLMeans:  malloc failure (weightsb)!" );
            return;
        }
    }
    else if( Az == 0 )
    {
        ds = new SDATA();
        ds->sums    = static_cast<double *>(AlignedMemory::alloc( vi.width * vi.height * sizeof(double), 16 ));
        ds->weights = static_cast<double *>(AlignedMemory::alloc( vi.width * vi.height * sizeof(double), 16 ));
        ds->wmaxs   = static_cast<double *>(AlignedMemory::alloc( vi.width * vi.height * sizeof(double), 16 ));
        if( !ds || !ds->sums || !ds->weights || !ds->wmaxs )
        {
            vsapi->setError( out, "TNLMeans:  malloc failure (ds)!" );
            return;
        }
    }

    gw = static_cast<double *>(AlignedMemory::alloc( Sxd * Syd * sizeof(double), 16 ));
    if( !gw ) vsapi->setError( out, "TNLMeans:  malloc failure (gw)!" );
    int w = 0, m, n;
    for( int j = -Sy; j <= Sy; ++j )
    {
        if( j < 0 )
            m = std::min( j + By, 0 );
        else
            m = std::max( j - By, 0 );
        for( int k = -Sx; k <= Sx; ++k )
        {
            if( k < 0 )
                n = std::min( k + Bx, 0 );
            else
                n = std::max( k - Bx, 0 );
            gw[w++] = std::exp( -((m * m + n * n) / (2 * a2)) );
        }
    }
}

TNLMeans::~TNLMeans()
{
    if( fc )     delete fc;
    if( dstPF )  delete dstPF;
    if( srcPFr ) delete srcPFr;
    if( gw )       AlignedMemory::free( gw );
    if( sumsb )    AlignedMemory::free( sumsb );
    if( weightsb ) AlignedMemory::free( weightsb );
    if( ds )
    {
        AlignedMemory::free( ds->sums );
        AlignedMemory::free( ds->weights );
        AlignedMemory::free( ds->wmaxs );
        delete ds;
    }
}

void TNLMeans::RequestFrame
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    for( int i = n - Az; i <= n + Az; ++i )
        vsapi->requestFrameFilter( mapn( i ), node, frame_ctx );
}

VSFrameRef *TNLMeans::CopyTo
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    const VSFrameRef *src = vsapi->getFrameFilter( mapn( n ), node, frame_ctx );
    VSFrameRef *dst = vsapi->newVideoFrame
    (
        vsapi->getFrameFormat( src ),
        vsapi->getFrameWidth ( src, 0 ),
        vsapi->getFrameHeight( src, 0 ),
        src, core
    );
    vsapi->freeFrame( src );
    dstPF->copyTo( dst, vsapi );
    return dst;
}

template < int ssd >
VSFrameRef *TNLMeans::GetFrameByMethod
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    if( Az )
    {
        if( Bx || By )
            return GetFrameWZB< ssd >( n, frame_ctx, core, vsapi );
        else
            return GetFrameWZ< ssd >( n, frame_ctx, core, vsapi );
    }
    else
    {
        if( Bx || By )
            return GetFrameWOZB< ssd >( n, frame_ctx, core, vsapi );
        else
            return GetFrameWOZ< ssd >( n, frame_ctx, core, vsapi );
    }
}

VSFrameRef *TNLMeans::GetFrame
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    if( ssd )
        return GetFrameByMethod< 1 >( n, frame_ctx, core, vsapi );
    else
        return GetFrameByMethod< 0 >( n, frame_ctx, core, vsapi );
}

template < int ssd >
VSFrameRef *TNLMeans::GetFrameWZ
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    fc->resetCacheStart( n - Az, n + Az );
    for( int i = n - Az; i <= n + Az; ++i )
    {
        nlFrame *nl = fc->frames[fc->getCachePos( i - n + Az )];
        if( nl->fnum != i )
        {
            const VSFrameRef *frame = vsapi->getFrameFilter( mapn( i ), node, frame_ctx );
            nl->pf->copyFrom( frame, vsapi );
            nl->setFNum( i );
            fc->clearDS( nl );
            vsapi->freeFrame( frame );
        }
    }
    const unsigned char **pfplut =
        static_cast<const unsigned char **>(AlignedMemory::alloc( fc->size * sizeof(const unsigned char *), 16 ));
    if( !pfplut ) { vsapi->setFilterError( "TNLMeans:  malloc failure (pfplut)!", frame_ctx ); return nullptr; }
    const SDATA **dslut =
        static_cast<const SDATA **>(AlignedMemory::alloc( fc->size * sizeof(SDATA *), 16 ));
    if( !dslut ) { vsapi->setFilterError( "TNLMeans:  malloc failure (dslut)!", frame_ctx ); return nullptr; }
    int **dsalut =
        static_cast<int **>(AlignedMemory::alloc( fc->size * sizeof(int *), 16 ));
    if( !dsalut ) { vsapi->setFilterError( "TNLMeans:  malloc failure (dsalut)!", frame_ctx ); return nullptr; }
    for( int i = 0; i < fc->size; ++i )
        dsalut[i] = fc->frames[fc->getCachePos( i )]->dsa;
    int *ddsa = dsalut[Az];
    PlanarFrame *srcPF = fc->frames[fc->getCachePos( Az )]->pf;
    const int startz = Az - std::min( n, Az );
    const int stopz  = Az + std::min( vi.numFrames - n - 1, Az );
    for( int plane = 0; plane < vi.format->numPlanes; ++plane )
    {
        const unsigned char *srcp = srcPF->GetPtr( plane );
        const unsigned char *pf2p = srcPF->GetPtr( plane );
        unsigned char *dstp = dstPF->GetPtr   ( plane );
        const int pitch     = dstPF->GetPitch ( plane );
        const int height    = dstPF->GetHeight( plane );
        const int width     = dstPF->GetWidth ( plane );
        const int heightm1  = height - 1;
        const int widthm1   = width  - 1;
        for( int i = 0; i < fc->size; ++i )
        {
            const int pos = fc->getCachePos( i );
            pfplut[i] = fc->frames[pos]->pf->GetPtr( plane );
            dslut [i] = fc->frames[pos]->ds[plane];
        }
        const SDATA *dds = dslut[Az];
        for( int y = 0; y < height; ++y )
        {
            const int startyt = std::max( y - Ay, 0 );
            const int stopy   = std::min( y + Ay, heightm1 );
            const int doffy   = y * width;
            for( int x = 0; x < width; ++x )
            {
                const int startxt = std::max( x - Ax, 0 );
                const int stopx   = std::min( x + Ax, widthm1 );
                const int doff = doffy + x;
                double *dsum    = &dds->sums   [doff];
                double *dweight = &dds->weights[doff];
                double *dwmax   = &dds->wmaxs  [doff];
                for( int z = startz; z <= stopz; ++z )
                {
                    if( ddsa[z] == 1 ) continue;
                    else ddsa[z] = 2;
                    const int starty = (z == Az) ? y : startyt;
                    const SDATA *cds = dslut[z];
                    int *cdsa = dsalut[z];
                    const unsigned char *pf1p = pfplut[z];
                    for( int u = starty; u <= stopy; ++u )
                    {
                        const int startx = (u == y && z == Az) ? x+1 : startxt;
                        const int yT = -std::min( std::min( Sy, u ), y );
                        const int yB =  std::min( std::min( Sy, heightm1 - u ), heightm1 - y );
                        const unsigned char *s1_saved = pf1p + (u+yT)*pitch;
                        const unsigned char *s2_saved = pf2p + (y+yT)*pitch + x;
                        const double *gw_saved = gw+(yT+Sy)*Sxd+Sx;
                        const int pf1pl = u*pitch;
                        const int coffy = u*width;
                        for( int v = startx; v <= stopx; ++v )
                        {
                            const int coff = coffy + v;
                            double *csum    = &cds->sums   [coff];
                            double *cweight = &cds->weights[coff];
                            double *cwmax   = &cds->wmaxs  [coff];
                            const int xL = -std::min( std::min( Sx, v ), x );
                            const int xR =  std::min( std::min( Sx, widthm1 - v ), widthm1 - x );
                            const unsigned char *s1 = s1_saved + v;
                            const unsigned char *s2 = s2_saved;
                            const double *gwT = gw_saved;
                            double diff = 0.0, gweights = 0.0;
                            for( int j = yT; j <= yB; ++j )
                            {
                                for( int k = xL; k <= xR; ++k )
                                {
                                    diff     += ssd ? GetSSD( s1, s2, gwT, k ) : GetSAD( s1, s2, gwT, k );
                                    gweights += gwT[k];
                                }
                                s1  += pitch;
                                s2  += pitch;
                                gwT += Sxd;
                            }
                            const double weight = ssd ? GetSSDWeight( diff, gweights ) : GetSADWeight( diff, gweights );
                            *dweight += weight;
                            *dsum    += pf1p[pf1pl+v]*weight;
                            if( weight > *dwmax ) *dwmax = weight;
                            if( cdsa[Azdm1-z] != 1 )
                            {
                                *cweight += weight;
                                *csum    += srcp[x]*weight;
                                if( weight > *cwmax ) *cwmax = weight;
                            }
                        }
                    }
                }
                const double wmax = *dwmax <= std::numeric_limits<double>::epsilon() ? 1.0 : *dwmax;
                *dsum    += srcp[x]*wmax;
                *dweight += wmax;
                dstp[x] = std::max( std::min( int(((*dsum) / (*dweight)) + 0.5), 255 ), 0 );
            }
            dstp += pitch;
            srcp += pitch;
        }
    }
    int j = fc->size - 1;
    for( int i = 0; i < fc->size; ++i, --j )
    {
        int *cdsa = fc->frames[fc->getCachePos( i )]->dsa;
        if( ddsa[i] == 2 ) ddsa[i] = cdsa[j] = 1;
    }
    AlignedMemory::free( dsalut );
    AlignedMemory::free( dslut );
    AlignedMemory::free( pfplut );
    return CopyTo( n, frame_ctx, core, vsapi );
}

template < int ssd >
VSFrameRef *TNLMeans::GetFrameWZB
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    fc->resetCacheStart( n - Az, n + Az );
    for( int i = n - Az; i <= n + Az; ++i )
    {
        nlFrame *nl = fc->frames[fc->getCachePos( i - n + Az )];
        if( nl->fnum != i )
        {
            const VSFrameRef *frame = vsapi->getFrameFilter( mapn( i ), node, frame_ctx );
            nl->pf->copyFrom( frame, vsapi );
            nl->setFNum( i );
            vsapi->freeFrame( frame );
        }
    }
    const unsigned char **pfplut =
        static_cast<const unsigned char **>(AlignedMemory::alloc( fc->size * sizeof(const unsigned char *), 16 ));
    if( !pfplut ) { vsapi->setFilterError( "TNLMeans:  malloc failure (pfplut)!", frame_ctx ); return nullptr; }
    PlanarFrame *srcPF = fc->frames[fc->getCachePos( Az )]->pf;
    const int startz = Az - std::min( n, Az );
    const int stopz  = Az + std::min( vi.numFrames - n - 1, Az );
    for( int plane = 0; plane < vi.format->numPlanes; ++plane )
    {
        const unsigned char *srcp = srcPF->GetPtr( plane );
        const unsigned char *pf2p = srcPF->GetPtr( plane );
        unsigned char *dstp     = dstPF->GetPtr   ( plane );
        const int      pitch    = dstPF->GetPitch ( plane );
        const int      height   = dstPF->GetHeight( plane );
        const int      width    = dstPF->GetWidth ( plane );
        const int      heightm1 = height - 1;
        const int      widthm1  = width  - 1;
        double *sumsb_saved    = sumsb    + Bx;
        double *weightsb_saved = weightsb + Bx;
        for( int i = 0; i < fc->size; ++i )
            pfplut[i] = fc->frames[fc->getCachePos( i )]->pf->GetPtr( plane );
        for( int y = By; y < height + By; y += Byd )
        {
            const int starty = std::max( y - Ay, By );
            const int stopy  = std::min( y + Ay, heightm1 - std::min( By, heightm1 - y ) );
            const int yTr    = std::min( Byd, height - y + By );
            for( int x = Bx; x < width + Bx; x += Bxd )
            {
                std::memset( sumsb,    0, Bxa * sizeof(double) );
                std::memset( weightsb, 0, Bxa * sizeof(double) );
                double wmax = 0.0;
                const int startx = std::max( x - Ax, Bx );
                const int stopx  = std::min( x + Ax, widthm1 - std::min( Bx, widthm1 - x ) );
                const int xTr    = std::min( Bxd,  width - x + Bx );
                for( int z = startz; z <= stopz; ++z )
                {
                    const unsigned char *pf1p = pfplut[z];
                    for( int u = starty; u <= stopy; ++u )
                    {
                        const int yT  = -std::min( std::min( Sy, u ), y );
                        const int yB  =  std::min( std::min( Sy, heightm1 - u ), heightm1 - y );
                        const int yBb =  std::min( std::min( By, heightm1 - u ), heightm1 - y );
                        const unsigned char *s1_saved  = pf1p + (u+yT)*pitch;
                        const unsigned char *s2_saved  = pf2p + (y+yT)*pitch + x;
                        const unsigned char *sbp_saved = pf1p + (u-By)*pitch;
                        const double *gw_saved = gw+(yT+Sy)*Sxd+Sx;
                        //const int pf1pl = u*pitch;
                        for( int v = startx; v <= stopx; ++v )
                        {
                            if( z == Az && u == y && v == x ) continue;
                            const int xL = -std::min( std::min( Sx, v ), x );
                            const int xR =  std::min( std::min( Sx, widthm1 - v), widthm1 - x );
                            const unsigned char *s1 = s1_saved + v;
                            const unsigned char *s2 = s2_saved;
                            const double *gwT = gw_saved;
                            double diff = 0.0, gweights = 0.0;
                            for( int j = yT; j <= yB; ++j )
                            {
                                for( int k = xL; k <= xR; ++k )
                                {
                                    diff     += ssd ? GetSSD( s1, s2, gwT, k ) : GetSAD( s1, s2, gwT, k );
                                    gweights += gwT[k];
                                }
                                s1  += pitch;
                                s2  += pitch;
                                gwT += Sxd;
                            }
                            const double weight = ssd ? GetSSDWeight( diff, gweights ) : GetSADWeight( diff, gweights );
                            const int xRb = std::min( std::min( Bx, widthm1 - v ), widthm1 - x );
                            const unsigned char *sbp = sbp_saved + v;
                            double *sumsbT    = sumsb_saved;
                            double *weightsbT = weightsb_saved;
                            for( int j = -By; j <= yBb; ++j )
                            {
                                for( int k = -Bx; k <= xRb; ++k )
                                {
                                    sumsbT   [k] += sbp[k]*weight;
                                    weightsbT[k] += weight;
                                }
                                sbp       += pitch;
                                sumsbT    += Bxd;
                                weightsbT += Bxd;
                            }
                            if( weight > wmax ) wmax = weight;
                        }
                    }
                }
                const unsigned char *srcpT = srcp + x - Bx;
                      unsigned char *dstpT = dstp + x - Bx;
                double *sumsbTr    = sumsb;
                double *weightsbTr = weightsb;
                if( wmax <= std::numeric_limits<double>::epsilon() ) wmax = 1.0;
                for( int j = 0; j < yTr; ++j )
                {
                    for( int k = 0; k < xTr; ++k )
                    {
                        sumsbTr   [k] += srcpT[k]*wmax;
                        weightsbTr[k] += wmax;
                        dstpT     [k] = std::max( std::min( int((sumsbTr[k] / weightsbTr[k]) + 0.5), 255 ),0 );
                    }
                    srcpT      += pitch;
                    dstpT      += pitch;
                    sumsbTr    += Bxd;
                    weightsbTr += Bxd;
                }
            }
            dstp += pitch*Byd;
            srcp += pitch*Byd;
        }
    }
    AlignedMemory::free( pfplut );
    return CopyTo( n, frame_ctx, core, vsapi );
}

template < int ssd >
VSFrameRef *TNLMeans::GetFrameWOZ
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    const VSFrameRef *frame = vsapi->getFrameFilter( mapn( n ), node, frame_ctx );
    srcPFr->copyFrom( frame, vsapi );
    vsapi->freeFrame( frame );
    for( int plane = 0; plane < vi.format->numPlanes; ++plane )
    {
        const unsigned char *srcp = srcPFr->GetPtr( plane );
        const unsigned char *pfp  = srcPFr->GetPtr( plane );
        unsigned char *dstp = dstPF->GetPtr   ( plane );
        const int pitch     = dstPF->GetPitch ( plane );
        const int height    = dstPF->GetHeight( plane );
        const int width     = dstPF->GetWidth ( plane );
        const int heightm1  = height - 1;
        const int widthm1   = width  - 1;
        std::memset( ds->sums,    0, height * width * sizeof(double) );
        std::memset( ds->weights, 0, height * width * sizeof(double) );
        std::memset( ds->wmaxs,   0, height * width * sizeof(double) );
        for( int y = 0; y < height; ++y )
        {
            const int stopy = std::min( y + Ay, heightm1 );
            const int doffy = y * width;
            for( int x = 0; x < width; ++x )
            {
                const int startxt = std::max( x - Ax, 0 );
                const int stopx   = std::min( x + Ax, widthm1 );
                const int doff = doffy + x;
                double *dsum    = &ds->sums   [doff];
                double *dweight = &ds->weights[doff];
                double *dwmax   = &ds->wmaxs  [doff];
                for( int u = y; u <= stopy; ++u )
                {
                    const int startx = u == y ? x+1 : startxt;
                    const int yT = -std::min( std::min( Sy, u ), y );
                    const int yB =  std::min( std::min( Sy, heightm1 - u ), heightm1 - y );
                    const unsigned char *s1_saved = pfp + (u+yT)*pitch;
                    const unsigned char *s2_saved = pfp + (y+yT)*pitch + x;
                    const double *gw_saved = gw+(yT+Sy)*Sxd+Sx;
                    const int pfpl  = u * pitch;
                    const int coffy = u * width;
                    for( int v = startx; v <= stopx; ++v )
                    {
                        const int coff = coffy+v;
                        double *csum    = &ds->sums   [coff];
                        double *cweight = &ds->weights[coff];
                        double *cwmax   = &ds->wmaxs  [coff];
                        const int xL = -std::min( std::min( Sx, v ), x );
                        const int xR =  std::min( std::min( Sx, widthm1 - v ), widthm1 - x );
                        const unsigned char *s1 = s1_saved + v;
                        const unsigned char *s2 = s2_saved;
                        const double *gwT = gw_saved;
                        double diff = 0.0, gweights = 0.0;
                        for( int j = yT; j <= yB; ++j )
                        {
                            for( int k = xL; k <= xR; ++k )
                            {
                                diff     += ssd ? GetSSD( s1, s2, gwT, k ) : GetSAD( s1, s2, gwT, k );
                                gweights += gwT[k];
                            }
                            s1  += pitch;
                            s2  += pitch;
                            gwT += Sxd;
                        }
                        const double weight = ssd ? GetSSDWeight( diff, gweights ) : GetSADWeight( diff, gweights );
                        *cweight += weight;
                        *dweight += weight;
                        *csum += srcp[x]     * weight;
                        *dsum += pfp[pfpl+v] * weight;
                        if( weight > *cwmax ) *cwmax = weight;
                        if( weight > *dwmax ) *dwmax = weight;
                    }
                }
                const double wmax = *dwmax <= std::numeric_limits<double>::epsilon() ? 1.0 : *dwmax;
                *dsum    += srcp[x]*wmax;
                *dweight += wmax;
                dstp[x] = std::max( std::min( int(((*dsum) / (*dweight)) + 0.5), 255 ), 0 );
            }
            dstp += pitch;
            srcp += pitch;
        }
    }
    return CopyTo( n, frame_ctx, core, vsapi );
}

template < int ssd >
VSFrameRef *TNLMeans::GetFrameWOZB
(
    int             n,
    VSFrameContext *frame_ctx,
    VSCore         *core,
    const VSAPI    *vsapi
)
{
    const VSFrameRef *frame = vsapi->getFrameFilter( mapn( n ), node, frame_ctx );
    srcPFr->copyFrom( frame, vsapi );
    vsapi->freeFrame( frame );
    for( int plane = 0; plane < vi.format->numPlanes; ++plane )
    {
        const unsigned char *srcp = srcPFr->GetPtr( plane );
        const unsigned char *pfp  = srcPFr->GetPtr( plane );
        unsigned char *dstp = dstPF->GetPtr   ( plane );
        const int pitch     = dstPF->GetPitch ( plane );
        const int height    = dstPF->GetHeight( plane );
        const int width     = dstPF->GetWidth ( plane );
        const int heightm1  = height - 1;
        const int widthm1   = width  - 1;
        double *sumsb_saved    = sumsb    + Bx;
        double *weightsb_saved = weightsb + Bx;
        for( int y = By; y < height + By; y += Byd )
        {
            const int starty = std::max( y - Ay, By );
            const int stopy  = std::min( y + Ay, heightm1 - std::min( By, heightm1 - y ) );
            const int yTr    = std::min( Byd, height - y + By );
            for( int x = Bx; x < width + Bx; x += Bxd )
            {
                std::memset( sumsb,    0, Bxa * sizeof(double) );
                std::memset( weightsb, 0, Bxa * sizeof(double) );
                double wmax = 0.0;
                const int startx = std::max( x - Ax, Bx );
                const int stopx  = std::min( x + Ax, widthm1 - std::min( Bx, widthm1 - x ) );
                const int xTr    = std::min( Bxd, width - x + Bx );
                for( int u = starty; u <= stopy; ++u )
                {
                    const int yT  = -std::min( std::min( Sy, u ), y );
                    const int yB  =  std::min( std::min( Sy, heightm1 - u ), heightm1 - y );
                    const int yBb =  std::min( std::min( By, heightm1 - u ), heightm1 - y );
                    const unsigned char *s1_saved  = pfp + (u+yT)*pitch;
                    const unsigned char *s2_saved  = pfp + (y+yT)*pitch + x;
                    const unsigned char *sbp_saved = pfp + (u-By)*pitch;
                    const double *gw_saved = gw+(yT+Sy)*Sxd+Sx;
                    for( int v = startx; v <= stopx; ++v )
                    {
                        if (u == y && v == x) continue;
                        const int xL = -std::min( std::min( Sx, v ), x );
                        const int xR =  std::min( std::min( Sx, widthm1 - v ), widthm1 - x );
                        const unsigned char *s1 = s1_saved + v;
                        const unsigned char *s2 = s2_saved;
                        const double *gwT = gw_saved;
                        double diff = 0.0, gweights = 0.0;
                        for(int j = yT; j <= yB; ++j )
                        {
                            for( int k = xL; k <= xR; ++k )
                            {
                                diff     += ssd ? GetSSD( s1, s2, gwT, k ) : GetSAD( s1, s2, gwT, k );
                                gweights += gwT[k];
                            }
                            s1  += pitch;
                            s2  += pitch;
                            gwT += Sxd;
                        }
                        const double weight = ssd ? GetSSDWeight( diff, gweights ) : GetSADWeight( diff, gweights );
                        const int xRb = std::min( std::min( Bx, widthm1 - v ), widthm1 - x );
                        const unsigned char *sbp = sbp_saved + v;
                        double *sumsbT    = sumsb_saved;
                        double *weightsbT = weightsb_saved;
                        for( int j = -By; j <= yBb; ++j )
                        {
                            for( int k = -Bx; k <= xRb; ++k )
                            {
                                sumsbT   [k] += sbp[k]*weight;
                                weightsbT[k] += weight;
                            }
                            sumsbT    += Bxd;
                            weightsbT += Bxd;
                            sbp += pitch;
                        }
                        if( weight > wmax ) wmax = weight;
                    }
                }
                const unsigned char *srcpT = srcp + x - Bx;
                      unsigned char *dstpT = dstp + x - Bx;
                double *sumsbTr    = sumsb;
                double *weightsbTr = weightsb;
                if( wmax <= std::numeric_limits<double>::epsilon() ) wmax = 1.0;
                for( int j = 0; j < yTr; ++j )
                {
                    for( int k = 0; k < xTr; ++k )
                    {
                        sumsbTr   [k] += srcpT[k]*wmax;
                        weightsbTr[k] += wmax;
                        dstpT     [k] = std::max( std::min( int((sumsbTr[k] / weightsbTr[k]) + 0.5), 255 ), 0 );
                    }
                    srcpT += pitch;
                    dstpT += pitch;
                    sumsbTr    += Bxd;
                    weightsbTr += Bxd;
                }
            }
            dstp += pitch*Byd;
            srcp += pitch*Byd;
        }
    }
    return CopyTo( n, frame_ctx, core, vsapi );
}

int TNLMeans::mapn( int n )
{
    if( n < 0 ) return 0;
    if( n >= vi.numFrames ) return vi.numFrames - 1;
    return n;
}

nlFrame::nlFrame( bool _useblocks, int _size, const VSVideoInfo &vi )
{
    fnum = -20;
    pf   = new PlanarFrame( vi );
    ds   = nullptr;
    dsa  = nullptr;
    if( !_useblocks )
    {
        ds = static_cast<SDATA **>(std::malloc( 3 * sizeof(SDATA *) ));
        for( int i = 0; i < 3; ++i )
        {
            ds[i] = new SDATA();
            ds[i]->sums    = static_cast<double *>(AlignedMemory::alloc( pf->GetHeight( i ) * pf->GetWidth( i ) * sizeof(double), 16 ));
            ds[i]->weights = static_cast<double *>(AlignedMemory::alloc( pf->GetHeight( i ) * pf->GetWidth( i ) * sizeof(double), 16 ));
            ds[i]->wmaxs   = static_cast<double *>(AlignedMemory::alloc( pf->GetHeight( i ) * pf->GetWidth( i ) * sizeof(double), 16 ));
        }
        dsa = static_cast<int *>(std::malloc( _size * sizeof(int) ));
        for( int i = 0; i < _size; ++i ) dsa[i] = 0;
    }
}

nlFrame::~nlFrame()
{
    if( pf ) delete pf;
    if( ds )
    {
        for( int i = 0; i < 3; ++i )
        {
            AlignedMemory::free( ds[i]->sums );
            AlignedMemory::free( ds[i]->weights );
            AlignedMemory::free( ds[i]->wmaxs );
            delete ds[i];
        }
        std::free( ds );
    }
    if( dsa ) std::free( dsa );
}

void nlFrame::setFNum( int i )
{
    fnum = i;
}

nlCache::nlCache( int _size, bool _useblocks, const VSVideoInfo &vi )
{
    frames = nullptr;
    start_pos = size = -20;
    if( _size > 0 )
    {
        start_pos = 0;
        size = _size;
        frames = static_cast<nlFrame **>(std::malloc( size * sizeof(nlFrame *) ));
        std::memset( frames, 0, size * sizeof(nlFrame *) );
        for( int i = 0; i < size; ++i )
            frames[i] = new nlFrame( _useblocks, _size, vi );
    }
}

nlCache::~nlCache()
{
    if( frames )
    {
        for( int i = 0; i < size; ++i )
            if( frames[i] ) delete frames[i];
        std::free( frames );
    }
}

void nlCache::resetCacheStart( int first, int last )
{
    for( int j = first; j <= last; ++j )
        for( int i = 0; i < size; ++i )
            if( frames[i]->fnum == j )
            {
                start_pos = i - j + first;
                if( start_pos < 0 )
                    start_pos += size;
                else if( start_pos >= size )
                    start_pos -= size;
                return;
            }
}

void nlCache::clearDS( nlFrame *nl )
{
    for( int i = 0; i < 3; ++i )
    {
        std::memset( nl->ds[i]->sums,    0, nl->pf->GetHeight( i ) * nl->pf->GetWidth( i ) * sizeof(double) );
        std::memset( nl->ds[i]->weights, 0, nl->pf->GetHeight( i ) * nl->pf->GetWidth( i ) * sizeof(double) );
        std::memset( nl->ds[i]->wmaxs,   0, nl->pf->GetHeight( i ) * nl->pf->GetWidth( i ) * sizeof(double) );
    }
    for( int i = 0; i < size; ++i ) nl->dsa[i] = 0;
}

int nlCache::getCachePos( int n )
{
    return (start_pos + n) % size;
}