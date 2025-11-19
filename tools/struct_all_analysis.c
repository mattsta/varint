/* Minimal test file to include all struct definitions for pahole analysis */
#include "varint.h"
#include "varintFOR.h"
#include "varintPFOR.h"
#include "varintFloat.h"
#include "varintAdaptive.h"
#include "varintBitmap.h"
#include "varintDict.h"
#include "varintDelta.h"
#include "perf.h"

int main(void) {
    /* Just reference the structs so compiler includes them */
    sizeof(varintFORMeta);
    sizeof(varintPFORMeta);
    sizeof(varintFloatMeta);
    sizeof(varintAdaptiveDataStats);
    sizeof(varintAdaptiveMeta);
    sizeof(varintBitmapStats);
    sizeof(varintBitmap);
    sizeof(varintBitmapIterator);
    sizeof(varintDict);
    sizeof(varintDictStats);
    sizeof(perfStateGlobal);
    sizeof(perfStateStat);
    sizeof(perfState);
    return 0;
}
