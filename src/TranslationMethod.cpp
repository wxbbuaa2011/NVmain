/*******************************************************************************
* Copyright (c) 2012-2013, The Microsystems Design Labratory (MDL)
* Department of Computer Science and Engineering, The Pennsylvania State University
* All rights reserved.
* 
* This source code is part of NVMain - A cycle accurate timing, bit accurate
* energy simulator for both volatile (e.g., DRAM) and nono-volatile memory
* (e.g., PCRAM). The source code is free and you can redistribute and/or
* modify it by providing that the following conditions are met:
* 
*  1) Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
* 
*  2) Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the documentation
*     and/or other materials provided with the distribution.
* 
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
* 
* Author list: 
*   Matt Poremba    ( Email: mrp5060 at psu dot edu 
*                     Website: http://www.cse.psu.edu/~poremba/ )
*   Tao Zhang       ( Email: tzz106 at cse dot psu dot edu 
*                     Website: http://www.cse.psu.edu/~tzz106/ )
*******************************************************************************/

#include <iostream>
#include <cstring>
#include "src/TranslationMethod.h"

using namespace NVM;

TranslationMethod::TranslationMethod( )
{
    /* Set some default translation method:
     *
     * The order is channel - rank - row - bank - col from MSB to LSB.
     * The method is for a 256 MB memory => 29 bits total.
     * The bits widths for each are 1 - 1 - 16 - 3 - 8
     */
    SetBitWidths( 16, 8, 3, 1, 1 );
    SetOrder( 3, 1, 2, 4, 5 );
}

TranslationMethod::~TranslationMethod( )
{
}

void TranslationMethod::SetBitWidths( unsigned int rowBits, unsigned int colBits, unsigned int bankBits,
				      unsigned int rankBits, unsigned int channelBits )
{
    if( colBits < 8 )
        std::cout << "NVMain: Column bits must be greater than or equal to the maximum burst length.\n";

    bitWidths[MEM_ROW] = rowBits;
    bitWidths[MEM_COL] = colBits;
    bitWidths[MEM_BANK] = bankBits;
    bitWidths[MEM_RANK] = rankBits;
    bitWidths[MEM_CHANNEL] = channelBits;
}

void TranslationMethod::SetOrder( int row, int col, int bank, int rank, int channel )
{
    if( row == col || row == bank || row == rank || row == channel
        || col == bank || col == rank || col == channel
        || bank == rank || bank == channel || rank == channel )
    {
        std::cout << "Translation Method: Orders are not unique!" << std::endl;
    }

    order[MEM_ROW] = row - 1;
    order[MEM_COL] = col - 1;
    order[MEM_BANK] = bank - 1;
    order[MEM_RANK] = rank - 1;
    order[MEM_CHANNEL] = channel - 1;
}

void TranslationMethod::SetCount( uint64_t rows, uint64_t cols, 
                                  uint64_t banks, uint64_t ranks, uint64_t channels )
{
    count[MEM_ROW] = rows;
    count[MEM_COL] = cols;
    count[MEM_BANK] = banks;
    count[MEM_RANK] = ranks;
    count[MEM_CHANNEL] = channels;
}

void TranslationMethod::GetBitWidths( unsigned int *rowBits, unsigned int *colBits, unsigned int *bankBits,
				      unsigned int *rankBits, unsigned int *channelBits )
{
    *rowBits = bitWidths[MEM_ROW];
    *colBits = bitWidths[MEM_COL];
    *bankBits = bitWidths[MEM_BANK];
    *rankBits = bitWidths[MEM_RANK];
    *channelBits = bitWidths[MEM_CHANNEL];
}

void TranslationMethod::GetOrder( int *row, int *col, int *bank, int *rank, int *channel )
{
    *row = order[MEM_ROW];
    *col = order[MEM_COL];
    *bank = order[MEM_BANK];
    *rank = order[MEM_RANK];
    *channel = order[MEM_CHANNEL];
}

void TranslationMethod::GetCount( uint64_t *rows, uint64_t *cols, uint64_t *banks, 
                                  uint64_t *ranks, uint64_t *channels )
{
    *rows = count[MEM_ROW];
    *cols = count[MEM_COL];
    *banks = count[MEM_BANK];
    *ranks = count[MEM_RANK];
    *channels = count[MEM_CHANNEL];
}

/*
 * Set the address mapping scheme
 * "R"-Row, "C"-Column, "BK"-Bank, "RK"-Rank, "CH"-Channel
 */
void TranslationMethod::SetAddressMappingScheme( std::string scheme )
{
    /* maximize row buffer hit */
    char addrMappingScheme[15]; 
    char *addrParser, *savePtr;

    strcpy( addrMappingScheme, (char*)scheme.c_str( ) );

    int row, col, bank, rank, channel;
    row = col = bank = rank = channel = 0;
    int currentOrder = 5;

    for( addrParser = strtok_r( addrMappingScheme, ":", &savePtr );
            addrParser ; addrParser = strtok_r( NULL, ":", &savePtr ) )
    {
        if( !strcmp( addrParser, "R" ) )
            row = currentOrder;
        else if( !strcmp( addrParser, "C" ) )
            col = currentOrder;
        else if( !strcmp( addrParser, "BK" ) )
            bank = currentOrder;
        else if( !strcmp( addrParser, "RK" ) )
            rank = currentOrder;
        else if( !strcmp( addrParser, "CH" ) )
            channel = currentOrder;
        else
            std::cerr << "NVMain Error: unrecognized address mapping scheme: " 
                << scheme << std::endl;

        /* move to next item */
        currentOrder--;
        if( currentOrder < 0 )
            std::cerr << "NVMain Error: invalid address mapping scheme: " 
                << scheme << std::endl;
    }
    
    SetOrder( row, col, bank, rank, channel );
    std::cout << "NVMain: the address mapping order is " << std::endl
        << "\tRow " << row << std::endl
        << "\tColumn " << col << std::endl
        << "\tBank " << bank << std::endl
        << "\tRank " << rank << std::endl
        << "\tChannel " << channel << std::endl;
}
