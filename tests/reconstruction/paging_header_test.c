#include "paging.h"
_Static_assert(sizeof(((ScannerReconstruction_PagingStats*)0)->logical_chunks)==8,"64-bit counts");
_Static_assert(sizeof(((ScannerReconstruction_PagingStats*)0)->requires_replay)==4,"32-bit flags");
void paging_c_signature_check(void) {
    Tango3DR_Status (*enable)(Tango3DR_ReconstructionContext,const char*,uint64_t,uint64_t,uint32_t)
        = ScannerReconstruction_enablePaging;
    Tango3DR_Status (*stats)(Tango3DR_ReconstructionContext,ScannerReconstruction_PagingStats*)
        = ScannerReconstruction_getPagingStats;
    (void)enable; (void)stats;
}
