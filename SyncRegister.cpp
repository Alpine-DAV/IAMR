//BL_COPYRIGHT_NOTICE

//
// $Id: SyncRegister.cpp,v 1.61 1999-08-01 15:12:37 almgren Exp $
//

#ifdef BL_USE_NEW_HFILES
#include <cstdlib>
#include <cstring>
#include <cstdio>
#else
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#endif

#include <BC_TYPES.H>
#include <SyncRegister.H>

#include <NAVIERSTOKES_F.H>
#ifndef FORT_HGC2N
#include <PROJECTION_F.H>
#endif

#include <SYNCREG_F.H>

//#define BL_SYNC_DEBUG 1

#ifdef BL_SYNC_DEBUG
static
Real
Norm_0 (const SyncRegister& br)
{
    Real r = 0;
    for (OrientationIter face; face; ++face)
    {
        for (ConstFabSetIterator cmfi(br[face()]); cmfi.isValid(); ++cmfi)
        {
            Real s = cmfi->norm(cmfi->box(), 0, 0, cmfi->nComp());
            r = (r > s) ? r : s;
        }
    }
    ParallelDescriptor::ReduceRealMax(r,ParallelDescriptor::IOProcessorNumber());
    return r;
}

static
Real
Norm_1 (const SyncRegister& br)
{
    Real r = 0;
    for (OrientationIter face; face; ++face)
    {
        for (ConstFabSetIterator cmfi(br[face()]); cmfi.isValid(); ++cmfi)
        {
            r += cmfi->norm(cmfi->box(), 1, 0, cmfi->nComp());
        }
    }
    ParallelDescriptor::ReduceRealSum(r,ParallelDescriptor::IOProcessorNumber());
    return r;
}

static
void
WriteNorms (const SyncRegister& sr,
            const char*         msg)
{
    Real n_0 = Norm_0(sr);
    Real n_1 = Norm_1(sr);
    if (ParallelDescriptor::IOProcessor())
    {
        cout << "*** " << msg << ": Norm_0: " << n_0 << endl;
        cout << "*** " << msg << ": Norm_1: " << n_1 << endl;
    }
}
#else
inline
void
WriteNorms (const SyncRegister&, const char*)
{}
#endif /*BL_SYNC_DEBUG*/


//
// Structure used by some SyncRegister functions.
//

struct SRRec
{
    SRRec (Orientation    face,
           int            index,
           const IntVect& shift)
        :
        m_shift(shift),
        m_idx(index),
        m_face(face)
    {}

    SRRec (Orientation face,
           int         index)
        :
        m_idx(index),
        m_face(face)
    {}

    FillBoxId   m_fbid;
    IntVect     m_shift;
    int         m_idx;
    Orientation m_face;
};

SyncRegister::SyncRegister ()
{
    fine_level = -1;
    ratio      = IntVect::TheUnitVector();
    ratio.scale(-1);
}

SyncRegister::SyncRegister (const BoxArray& fine_boxes,
                            const IntVect&  ref_ratio,
                            int             fine_lev)
{
    ratio = IntVect::TheUnitVector();
    ratio.scale(-1);
    define(fine_boxes,ref_ratio,fine_lev);
}

void
SyncRegister::define (const BoxArray& fine_boxes,
                      const IntVect&  ref_ratio,
                      int             fine_lev)
{
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
        BL_ASSERT(ratio[dir] == -1);

    BL_ASSERT(!grids.ready());
    BL_ASSERT(fine_boxes.isDisjoint());

    ratio      = ref_ratio;
    fine_level = fine_lev;
 
    grids.define(fine_boxes);
    grids.coarsen(ratio);

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        //
        // Construct BoxArrays for the FabSets.
        //
        const Orientation lo = Orientation(dir,Orientation::low);
        const Orientation hi = Orientation(dir,Orientation::high);

        BoxArray loBA(grids.length());
        BoxArray hiBA(grids.length());

        for (int k = 0; k < grids.length(); k++)
        {
            Box ndbox = ::surroundingNodes(grids[k]);

            Box nd_lo = ndbox;
            nd_lo.setRange(dir,ndbox.smallEnd(dir),1);
            loBA.set(k,nd_lo);

            Box nd_hi = ndbox;
            nd_hi.setRange(dir,ndbox.bigEnd(dir),1);
            hiBA.set(k,nd_hi);
        }
        //
        // Define the FabSets.
        //
        bndry[lo].define(loBA,1);
        bndry_mask[lo].define(loBA,1);
        bndry[hi].define(hiBA,1);
        bndry_mask[hi].define(hiBA,1);
    }
}

SyncRegister::~SyncRegister () {}

Real
SyncRegister::sum ()
{
    //
    // Sum values in all registers.  Note that overlap is counted twice.
    //
    Box bb = grids[0];

    for (int k = 1; k < grids.length(); k++)
        bb.minBox(grids[k]);

    bb.surroundingNodes();

    FArrayBox bfab(bb,1);

    bfab.setVal(0);

    for (OrientationIter face; face; ++face)
    {
        for (FabSetIterator fsi(bndry[face()]); fsi.isValid(); ++fsi)
        {
            bfab.copy(fsi());
        }
    }
    Real tempSum = bfab.sum(0,1);

    ParallelDescriptor::ReduceRealSum(tempSum);

    return tempSum;
}

//
// If periodic, copy the values from sync registers onto the nodes of the
// rhs which are not covered by sync registers through periodic shifts.
//

void
SyncRegister::copyPeriodic (const Geometry& geom,
                            const Box&      domain,
                            MultiFab&       rhs) const
{
    if (!geom.isAnyPeriodic()) return;

    const int            MyProc = ParallelDescriptor::MyProc();
    Array<IntVect>       pshifts(27);
    vector<SRRec>        srrec;
    FabSetCopyDescriptor fscd;
    FabSetId             fsid[2*BL_SPACEDIM];

    for (OrientationIter face; face; ++face)
    {
        fsid[face()] = fscd.RegisterFabSet((FabSet*) &bndry[face()]);

        for (MultiFabIterator mfi(rhs); mfi.isValid(); ++mfi)
        {
            for (int j = 0; j < grids.length(); j++)
            {
                geom.periodicShift(domain,bndry[face()].fabbox(j),pshifts);

                for (int iiv = 0; iiv < pshifts.length(); iiv++)
                {
                    Box sbox = bndry[face()].fabbox(j) + pshifts[iiv];

                    if (sbox.intersects(mfi().box()))
                    {
                        sbox &= mfi().box();
                        sbox -= pshifts[iiv];

                        SRRec sr(face(), mfi.index(), pshifts[iiv]);

                        sr.m_fbid = fscd.AddBox(fsid[face()],
                                                sbox,
                                                0,
                                                j,
                                                0,
                                                0,
                                                mfi().nComp());

                        BL_ASSERT(sr.m_fbid.box() == sbox);

                        srrec.push_back(sr);
                    }
                }
            }
        }
    }

    fscd.CollectData();

    for (int i = 0; i < srrec.size(); i++)
    {
        BL_ASSERT(rhs.DistributionMap()[srrec[i].m_idx] == MyProc);

        FArrayBox& fab = rhs[srrec[i].m_idx];

        Box dstbox = srrec[i].m_fbid.box() + srrec[i].m_shift;

        fscd.FillFab(fsid[srrec[i].m_face], srrec[i].m_fbid, fab, dstbox);
    }
}

void
SyncRegister::multByBndryMask (MultiFab& rhs) const
{
    const int            MyProc = ParallelDescriptor::MyProc();
    FabSetCopyDescriptor fscd;
    FabSetId             fsid[2*BL_SPACEDIM];
    vector<SRRec>        srrec;
    FArrayBox            tmpfab;

    for (OrientationIter face; face; ++face)
    {
        fsid[face()] = fscd.RegisterFabSet((FabSet*) &bndry_mask[face()]);

        for (MultiFabIterator mfi(rhs); mfi.isValid(); ++mfi)
        {
            for (int j = 0; j < grids.length(); j++)
            {
                if (mfi().box().intersects(bndry_mask[face()].fabbox(j)))
                {
                    Box intersect = mfi().box() & bndry_mask[face()].fabbox(j);

                    SRRec sr(face(), mfi.index());

                    sr.m_fbid = fscd.AddBox(fsid[face()],
                                            intersect,
                                            0,
                                            j,
                                            0,
                                            0,
                                            mfi().nComp());

                    BL_ASSERT(sr.m_fbid.box() == intersect);

                    srrec.push_back(sr);
                }
            }
        }
    }

    fscd.CollectData();

    for (int i = 0; i < srrec.size(); i++)
    {
        const Box& bx = srrec[i].m_fbid.box();

        BL_ASSERT(rhs.DistributionMap()[srrec[i].m_idx] == MyProc);

        FArrayBox& rhs_fab = rhs[srrec[i].m_idx];

        tmpfab.resize(bx, rhs_fab.nComp());

        fscd.FillFab(fsid[srrec[i].m_face], srrec[i].m_fbid, tmpfab, bx);

        rhs_fab.mult(tmpfab, bx, bx, 0, 0, rhs_fab.nComp());
    }
}

void
SyncRegister::InitRHS (MultiFab&       rhs,
                       const Geometry& geom,
                       const BCRec*    phys_bc)
{
    rhs.setVal(0);

    Box domain = ::surroundingNodes(geom.Domain());

    const int MyProc = ParallelDescriptor::MyProc();

    FabSetCopyDescriptor fscd;

    Array<IntVect> pshifts(27);
    //
    // Fill Rhs From Bndry Registers.
    //
    // If periodic, copy the values from sync registers onto the nodes of the
    // rhs which are not covered by sync registers through periodic shifts.
    //
    copyPeriodic(geom,domain,rhs);
    //
    // Overwrite above-set values on all nodes covered by a sync register.
    //
    for (OrientationIter face; face; ++face)
    {
        bndry[face()].copyTo(rhs);
    }
    const int* phys_lo = phys_bc->lo();
    const int* phys_hi = phys_bc->hi();

    Box node_domain = ::surroundingNodes(geom.Domain());
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        if (!geom.isPeriodic(dir))
        {
            Box domlo(node_domain), domhi(node_domain);
            domlo.setRange(dir,node_domain.smallEnd(dir),1);
            domhi.setRange(dir,node_domain.bigEnd(dir),1);

            for (MultiFabIterator mfi(rhs); mfi.isValid(); ++mfi)
            {
                if (domlo.intersects(mfi.validbox()))
                {
                  Box blo = mfi.validbox() & domlo;
                  if (blo.ok() && phys_lo[dir] == Outflow)
                      mfi().mult(0.0,blo,0,1);
                }

                if (domhi.intersects(mfi.validbox())) 
                {
                  Box bhi = mfi.validbox() & domhi;
                  if (bhi.ok() && phys_hi[dir] == Outflow) 
                      mfi().mult(0.0,bhi,0,1);
                }
            }
        }
    }

    //
    // Set Up bndry_mask.
    //
    for (OrientationIter face; face; ++face)
    {
        bndry_mask[face()].setVal(0);
    }

    FArrayBox tmpfab;

    for (OrientationIter face; face; ++face)
    {
        for (FabSetIterator fsi(bndry_mask[face()]); fsi.isValid(); ++fsi)
        {
            Box mask_cells = ::enclosedCells(::grow(fsi().box(),1));

            tmpfab.resize(mask_cells,1);
            tmpfab.setVal(0);

            for (int n = 0; n < grids.length(); n++)
                if (mask_cells.intersects(grids[n]))
                {
                    tmpfab.setVal(1.0,(mask_cells & grids[n]),0,1);
                }
 
            if (geom.isAnyPeriodic())
            {
                geom.periodicShift(geom.Domain(),mask_cells,pshifts);

                for (int iiv = 0; iiv < pshifts.length(); iiv++)
                {
                    mask_cells += pshifts[iiv];

                    for (int n = 0; n < grids.length(); n++)
                    {
                        if (mask_cells.intersects(grids[n]))
                        {
                            Box intersect = mask_cells & grids[n];
                            intersect    -= pshifts[iiv];
                            tmpfab.setVal(1.0,intersect,0,1);
                        }
                    }

                    mask_cells -= pshifts[iiv];
                }
            }
            Real* mask_dat = fsi().dataPtr();
            const int* mlo = fsi().loVect(); 
            const int* mhi = fsi().hiVect();
            Real* cell_dat = tmpfab.dataPtr();
            const int* clo = tmpfab.loVect(); 
            const int* chi = tmpfab.hiVect();
        
            FORT_MAKEMASK(mask_dat,ARLIM(mlo),ARLIM(mhi),
                          cell_dat,ARLIM(clo),ARLIM(chi));
        }
    }

    //
    // Here double the cell contributions if at a non-periodic physical bdry.
    //
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        if (!geom.isPeriodic(dir))
        {
            Box domlo(node_domain), domhi(node_domain);

            domlo.setRange(dir,node_domain.smallEnd(dir),1);
            domhi.setRange(dir,node_domain.bigEnd(dir),1);

            for (OrientationIter face; face; ++face)
            {
                for (FabSetIterator fsi(bndry_mask[face()]); fsi.isValid(); ++fsi)
                {
                    if (domlo.intersects(fsi().box()))
                        fsi().mult(2.0,fsi().box() & domlo,0,1);

                    if (domhi.intersects(fsi().box()))
                        fsi().mult(2.0,fsi().box() & domhi,0,1);
                }
            }
        }
    }
    //
    // Here convert from sum of cell contributions to 0 or 1.
    //
    for (OrientationIter face; face; ++face)
    {
        for (FabSetIterator fsi(bndry_mask[face()]); fsi.isValid(); ++fsi)
        {
            Real* mask_dat  = fsi().dataPtr();
            const int* mlo  = fsi().loVect(); 
            const int* mhi  = fsi().hiVect();

            FORT_CONVERTMASK(mask_dat,ARLIM(mlo),ARLIM(mhi));
        }
    }

    multByBndryMask(rhs);

    WriteNorms(*this,"SyncRegister::InitRHS(E)");
}

enum HowToBuild { WITH_BOX, WITH_SURROUNDING_BOX };

static
void
BuildMFs (const MultiFab& mf,
          MultiFab        cloMF[BL_SPACEDIM],
          MultiFab        chiMF[BL_SPACEDIM],
          const IntVect&  ratio,
          HowToBuild      how)
{
    BL_ASSERT(how == WITH_BOX || how == WITH_SURROUNDING_BOX);

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        BoxArray cloBA(mf.length());
        BoxArray chiBA(mf.length());

        for (int i = 0; i < mf.length(); i++)
        {
            Box bx = mf.boxArray()[i];

            if (how == WITH_SURROUNDING_BOX)
                bx = ::surroundingNodes(mf.boxArray()[i]);

            Box tboxlo(bx), tboxhi(bx);

            tboxlo.setRange(dir,bx.smallEnd(dir),1);
            tboxhi.setRange(dir,bx.bigEnd(dir),1);

            cloBA.set(i,::coarsen(tboxlo,ratio));
            chiBA.set(i,::coarsen(tboxhi,ratio));
        }

        cloMF[dir].define(cloBA,1,0,Fab_allocate);
        chiMF[dir].define(chiBA,1,0,Fab_allocate);

        cloMF[dir].setVal(0);
        chiMF[dir].setVal(0);
    }
}

void
SyncRegister::incrementPeriodic (const Geometry& geom,
                                 const Box&      domain,
                                 const MultiFab& mf)
{
    if (!geom.isAnyPeriodic()) return;

    const int              MyProc = ParallelDescriptor::MyProc();
    const BoxArray&        mfBA   = mf.boxArray();
    Array<IntVect>         pshifts(27);
    vector<SRRec>          srrec;
    FArrayBox              tmpfab;
    MultiFabCopyDescriptor mfcd;
    MultiFabId             mfid = mfcd.RegisterFabArray((MultiFab*) &mf);

    for (OrientationIter face; face; ++face)
    {
        for (FabSetIterator fsi(bndry[face()]); fsi.isValid(); ++fsi)
        {
            for (int j = 0; j < mfBA.length(); j++)
            {
                geom.periodicShift(domain, mfBA[j], pshifts);

                for (int iiv = 0; iiv < pshifts.length(); iiv++)
                {
                    Box sbox = mfBA[j] + pshifts[iiv];

                    if (sbox.intersects(fsi().box()))
                    {
                        sbox &= fsi().box();
                        sbox -= pshifts[iiv];

                        SRRec sr(face(), fsi.index(), pshifts[iiv]);

                        sr.m_fbid = mfcd.AddBox(mfid,
                                                sbox,
                                                0,
                                                j,
                                                0,
                                                0,
                                                mf.nComp());

                        BL_ASSERT(sr.m_fbid.box() == sbox);

                        srrec.push_back(sr);
                    }
                }
            }
        }
    }

    mfcd.CollectData();

    for (int i = 0; i < srrec.size(); i++)
    {
        FabSet& fabset = bndry[srrec[i].m_face];

        BL_ASSERT(fabset.DistributionMap()[srrec[i].m_idx] == MyProc);

        tmpfab.resize(srrec[i].m_fbid.box(), mf.nComp());

        mfcd.FillFab(mfid, srrec[i].m_fbid, tmpfab);

        tmpfab.shift(srrec[i].m_shift);

        fabset[srrec[i].m_idx].plus(tmpfab, 0, 0, mf.nComp());
    }
}

void
SyncRegister::CrseInit  (MultiFab* Sync_resid_crse,
                         Real            mult)
{
    WriteNorms(*this,"SyncRegister::CrseInit(B)");

    setVal(0.);

    Sync_resid_crse->mult(mult);

    for (OrientationIter face; face; ++face)
    {
        bndry[face()].plusFrom(*Sync_resid_crse,0,0,0,1);
    }

    WriteNorms(*this,"SyncRegister::CrseInit(E)");
}

void
SyncRegister::CompAdd  (MultiFab* Sync_resid_fine,
                        const Geometry& fine_geom, 
                        const Geometry& crse_geom, 
                        const BCRec*    phys_bc,
                        const BoxArray& Pgrids,
                        Real            mult)
{
  Array<IntVect> pshifts(27);
  for (ConstMultiFabIterator mfi(*Sync_resid_fine); mfi.isValid(); ++mfi)
  {
    Box sync_box = mfi.validbox();
    for (int i = 0; i < Pgrids.length(); i++) 
    {
       if (sync_box.intersects(Pgrids[i])) 
         Sync_resid_fine->setVal(0.0,(sync_box&Pgrids[i]),0,1);

       fine_geom.periodicShift(sync_box, Pgrids[i], pshifts);
       for (int iiv = 0; iiv < pshifts.length(); iiv++)
       {
           Box overlap_per = Pgrids[i] + pshifts[iiv];
           overlap_per    &= sync_box;
           Sync_resid_fine->setVal(0.0,overlap_per,0,1);
       }
    }
  }
  FineAdd(Sync_resid_fine,fine_geom,crse_geom,phys_bc,mult);
}

void
SyncRegister::FineAdd  (MultiFab* Sync_resid_fine,
                        const Geometry& fine_geom, 
                        const Geometry& crse_geom, 
                        const BCRec*    phys_bc,
                        Real            mult)
{
    WriteNorms(*this,"SyncRegister::FineAdd(B)");

    MultiFab cloMF[BL_SPACEDIM], chiMF[BL_SPACEDIM];

    BuildMFs(*Sync_resid_fine,cloMF,chiMF,ratio,WITH_BOX);

    Sync_resid_fine->mult(mult);

    const int* phys_lo = phys_bc->lo();
    const int* phys_hi = phys_bc->hi();

    Box fine_node_domain = ::surroundingNodes(fine_geom.Domain());
    Box crse_node_domain = ::surroundingNodes(crse_geom.Domain());

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        if (!fine_geom.isPeriodic(dir))
        {
            //
            // Any RHS point on the physical bndry must here be halved
            // for any boundary but outflow or periodic.  
            //
            Box domlo(fine_node_domain), domhi(fine_node_domain);

            domlo.setRange(dir,fine_node_domain.smallEnd(dir),1);
            domhi.setRange(dir,fine_node_domain.bigEnd(dir),1);

            for (MultiFabIterator mfi(*Sync_resid_fine); mfi.isValid(); ++mfi)
            {
                if (domlo.intersects(mfi.validbox()))
                {
                  Box blo = mfi.validbox() & domlo;
                  if (blo.ok()) 
                    mfi().mult(0.5,blo,0,1);
                }

                if (domhi.intersects(mfi.validbox())) 
                {
                  Box bhi = mfi.validbox() & domhi;
                  if (bhi.ok()) 
                    mfi().mult(0.5,bhi,0,1);
                }

            }
        }
    }

    if (fine_geom.isAnyPeriodic())
      fine_geom.FillPeriodicBoundary(*Sync_resid_fine,false,false);

    int is_per;

    //
    // Coarsen edge values.
    //
    for (ConstMultiFabIterator mfi(*Sync_resid_fine); mfi.isValid(); ++mfi)
    {
        const int* resid_lo = mfi().box().loVect();
        const int* resid_hi = mfi().box().hiVect();

        for (int dir = 0; dir < BL_SPACEDIM; dir++)
        {

            FArrayBox& cfablo = cloMF[dir][mfi.index()];
            const Box& cboxlo = cfablo.box();
            const int* clo = cboxlo.loVect();
            const int* chi = cboxlo.hiVect();

            if (fine_geom.isPeriodic(dir) && 
                clo[dir] == crse_node_domain.smallEnd(dir)) 
            {
              is_per = 1;
            } else {
              is_per = 0;
            }

            FORT_SRCRSEREG(mfi().dataPtr(),ARLIM(resid_lo),ARLIM(resid_hi),
                           cfablo.dataPtr(),ARLIM(clo),ARLIM(chi),
                           clo,chi,&dir,ratio.getVect(),&is_per);

            FArrayBox& cfabhi = chiMF[dir][mfi.index()];
            const Box& cboxhi = cfabhi.box();
            clo = cboxhi.loVect();
            chi = cboxhi.hiVect();

            if (fine_geom.isPeriodic(dir) && 
                clo[dir] == crse_node_domain.bigEnd(dir)) 
            {
              is_per = 1;
            } else {
              is_per = 0;
            }

            FORT_SRCRSEREG(mfi().dataPtr(),ARLIM(resid_lo),ARLIM(resid_hi),
                           cfabhi.dataPtr(),ARLIM(clo),ARLIM(chi),
                           clo,chi,&dir,ratio.getVect(),&is_per);
        }
    }

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        if (!crse_geom.isPeriodic(dir))
        {
            //
            // Any RHS point on the physical bndry must here be doubled
            // for any boundary but outflow or periodic
            //
            Box domlo(crse_node_domain), domhi(crse_node_domain);

            domlo.setRange(dir,crse_node_domain.smallEnd(dir),1);
            domhi.setRange(dir,crse_node_domain.bigEnd(dir),1);

            for (MultiFabIterator mfi(*Sync_resid_fine); mfi.isValid(); ++mfi)
              for (int dir_cfab = 0; dir_cfab < BL_SPACEDIM; dir_cfab++) 
            {
                FArrayBox& cfablo = cloMF[dir_cfab][mfi.index()];
                const Box& cboxlo = cfablo.box();
                if (domlo.intersects(cboxlo))
                  cfablo.mult(2.0,cboxlo&domlo,0,1);
                if (domhi.intersects(cboxlo))
                  cfablo.mult(2.0,cboxlo&domhi,0,1);

                FArrayBox& cfabhi = chiMF[dir_cfab][mfi.index()];
                const Box& cboxhi = cfabhi.box();
                if (domlo.intersects(cboxhi))
                  cfabhi.mult(2.0,cboxhi&domlo,0,1);
                if (domhi.intersects(cboxhi))
                  cfabhi.mult(2.0,cboxhi&domhi,0,1);
            }
        }
    }

    //
    // Intersect and add to registers.
    //

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        for (OrientationIter face; face; ++face)
        {
            bndry[face()].plusFrom(cloMF[dir],0,0,0,1);
            bndry[face()].plusFrom(chiMF[dir],0,0,0,1);
        }
    }

    WriteNorms(*this,"SyncRegister::FineAdd(E)");
}

