#pragma once

#if defined(BOARD_WROOM)
    #include "Board_WROOM.h"
#elif defined(BOARD_S3)
    #include "Board_S3.h"
#else
    #error "Ingen hårdvaruprofil vald!"
#endif