//////////
//
// savetoparser.cpp
//
/////
//
// July 10, 2012
// Rick C. Hodgin
//
/////
//
// This software is released under the GPLv3.  See http://gplv3.fsf.org.
// It is free software under the terms granted in that license.
// Please feel free to use, modify, distribute copies of this
// software, provided you follow the terms of the GPLv3.
//
/////









#define _WIN32_WINNT				0x500
#define _CRT_SECURE_NO_WARNINGS		1		// Turn off annoying "sprintf/fopen may be unsafe" messages
	#include <windows.h>
	#include <stdio.h>
	#include <stdlib.h>
	#include <malloc.h>

typedef		unsigned __int64	u64;
typedef		unsigned long		u32;
typedef		unsigned short		u16;
typedef		unsigned char		u8;

typedef		LARGE_INTEGER		i64;
typedef		__int64				s64;
typedef		long				s32;
typedef		short				s16;
typedef		char				s8;

typedef		float				f32;
typedef		double				f64;

typedef		const s8			cs8;
typedef		const s16			cs16;
typedef		const s32			cs32;
typedef		const s64			cs64;

typedef		const u8			cu8;
typedef		const u16			cu16;
typedef		const u32			cu32;
typedef		const u64			cu64;

typedef		const f32			cf32;
typedef		const f64			cf64;

//////////
// Holds information during template expansion for sizing and re-sizing as components are added,
// ultimately used in a new SLine entry for the VFP-to-SQL conversion.
//////
	struct STemplate
	{
		s8*			buffer;					// Start of the buffer
		u32			totSize;				// Total allocated size of the buffer
		u32			currentLength;			// Offset into the buffer where the next character would go
	};

/////////
//
// Used for processing SAVE TO files
//
/////
	struct SSaveToVar
	{
		s8			name[11];		// First 10 of name, "FOO",00,00,00,00,00,00,00,[trailing NULL]
		u8			type;			// CH,N,I,etc, if lower-case, then variable name is long and is stored in the data area
		u8			unk1[1];		// 00
		u32			length;			// 00,00,00,18 (includes trailing null)
		u8			decimals;		// 00
		u8			unk2[7];		// 00,00,00,00,00,00,00
		u8			val;			// 03
		u8			unk3[6];		// 00,00,00,00,00,00
		// Total 32 bytes
	};

	// These formats immediately follow SSaveToVar
	struct SSaveToVarLongName
	{
		u16			length;			// Length of the name (bytes of name immediately follow)
	};

	struct SSaveToVarArray
	{
		u16			rows;			// 02,00
		u16			cols;			// 03,00
	};

	struct SSaveToVarFloat
	{
		f64			fVal;			// 8-byte floating point value
	};

	struct SSaveToVarLogical
	{
		u8			lVal;			// 8-bit indicator, 0=false, 1=true
	};

	struct SSaveToVarDate
	{
		f64			dVal;			// 8-byte date value
	};

	struct SSaveToVarDatetime
	{
		f64			dVal;			// 8-byte datetime value
	};




//////////
// Forward declarations
//////
	// Externally visible (see savetoparser.def)
	u32				process_save_to_file							(s8* tcInputFile, s8* tcOutputFile);

	// Internal functions
	void			iSaveTo_StoreName								(STemplate* builder, SSaveToVar* stv);
	SSaveToVar*		iSaveTo_StoreData								(STemplate* builder, SSaveToVar* stv, bool tlStoreName);
	void			iComputeDateFromJulianDayNumber					(u32 tnJulianDayNumber, s8* tcDate11);
	void			iComputeTimeFromFraction						(f32 tfSeconds,         s8* tcTime13);
	void			iLoadFileContents								(s8* tcInputFile, FILE** tfh, s8** tcData, u32* tnLength, u32* tnNumread);
	void			iAllocateAndInitializeAccumulationTemplate		(STemplate** templ);
	void			iAppendToTemplate								(STemplate* dst, const s8* src,	u32 length);
	void			iAppendToTemplate								(STemplate* dst, s8* src,		u32 length);
	void			iAppendToTemplateWhitespaces					(STemplate* dst, u32 tnCount);
	u32				iAppendToTemplateInteger						(STemplate* dst, s32 tnValue);
	void			iFreeAndReleaseAccumulationTemplate				(STemplate** templ, bool tbFreeBuffer);
	bool			iIsNeedleInHaystack								(s8* haystack, s32 haystackLength, s8* needle, s32 needleLegth);
	void			iLowercase										(s8* src, u32 length);
	s8				iLowerCharacter									(s8 ch);
	u32				iSwapEndian										(u32 value);
	void			iAppendToTemplateSkipNulls						(STemplate* dst, s8* src, u32 length);
	u32				iAppendToTemplateDouble							(STemplate* dst, f64 tfValue, u32 length, u32 decimals);
	void			iAppendToTemplateValidateBufferSize				(STemplate* dst, u32 length);
	u32				iSkipWhitespaces								(s8* source, u32* offset, u32 maxLength);
	u32				write_to_file									(s8* tcFilename, u32 tnOrigin, u32 tnOffset, s8* tcContent, u32 tnContentLength);




// For variable names, the known first and second characters
const s8	cgcKnownFirsts[]	= "tpgl";
const s8	cgcKnownSeconds[]	= "cnlibdtfy";




//////////
//
// Main Windows entry point
//
/////
    BOOL APIENTRY DllMain( HMODULE	hModule,
                           DWORD	ul_reason_for_call,
                           LPVOID	lpReserved
					     )
    {
	    switch (ul_reason_for_call)
	    {
	        case DLL_PROCESS_ATTACH:
                break;

	        case DLL_THREAD_ATTACH:
                break;

	        case DLL_THREAD_DETACH:
                break;

	        case DLL_PROCESS_DETACH:
                break;

	    }
	    return TRUE;
    }



//////////
//
// Called to parse the SAVE TO filename file, to read all variables
// and return them in a list of quote-comma delimited line items,
// such as:
//
//		(C:8)foo = whatever
//		(N:5,2)i = 12.03
//		lnArray[1,1](C:4) = foo
//		lnArray[1,2](N:1) = 0
//
//////
	u32 process_save_to_file(s8* tcInputFile, s8* tcOutputFile)
	{
		u32					lnLength, lnRow, lnCol;
		STemplate*			builder;
		// Holds loaded content
		s8*					lcData;
		u32					lnDataLength;
		SSaveToVar*			stv;
		SSaveToVarArray*	stva;


		//////////
		// Try to open the specified input file
		//////
			iLoadFileContents(tcInputFile, NULL, &lcData, &lnDataLength, NULL);
			if (lcData == NULL)
				return(0);
			// If we get here, we're good, we have our loaded buffer


		//////////
		// Allocate our builder accumulation buffer
		//////
			iAllocateAndInitializeAccumulationTemplate(&builder);


		//////////
		// Iterate through the data
		//////
			stv			= (SSaveToVar*)lcData;
			while ((u32)stv < (u32)lcData + lnDataLength && stv->name[0] != 0x1a/* VFP uses CHR(26) (which is hexadecimal 0x1a) as the terminator for the file (the CTRL+Z character)*/)
			{
				// Find out what type of variable it is
				if (stv->type == 'a' || stv->type == 'A')
				{
					// It's an array, special processing
					stva = (SSaveToVarArray*)(stv + 1);		// Array data comes immediately after initial header

					//////////
					// Store the name
					/////
						iSaveTo_StoreName(builder, stv);


					//////////
					// Store the array portion after the name above, so it's like:
					//	foo[5,5] - array
					//////
						iAppendToTemplate(builder, "[", 1);
						iAppendToTemplateInteger(builder, (s32)stva->rows);
						if (stva->cols != 0)
						{
							iAppendToTemplate(builder, ",", 1);
							iAppendToTemplateInteger(builder, (s32)stva->cols);
						}
						iAppendToTemplate(builder, "] - array", 9);
						// Append CR/LF after
						iAppendToTemplate(builder, "\r\n", 2);


					//////////
					// Now, move stv forward to the start of the next thing
					//////
						stv = (SSaveToVar*)((s8*)(stv+1) + sizeof(SSaveToVarArray));


					//////////
					// Repeat for each entry that will follow, for both dimensions of the array
					//////
						for (lnRow = 1; lnRow <= (u32)stva->rows; lnRow++)
						{
							// Store the array reference
							if (stva->cols == 0)
							{
								// No columns, single-dimension array
								// Store the name
								iSaveTo_StoreName(builder, stv);
								iAppendToTemplate(builder, "[", 1);
								iAppendToTemplateInteger(builder, lnRow);
								iAppendToTemplate(builder, "]", 1);

								// Store the data
								stv = iSaveTo_StoreData(builder, stv, false);

								// Append CR/LF after
								iAppendToTemplate(builder, "\r\n", 2);

							} else {
								// Two-dimensional array
								for (lnCol = 1; lnCol <= (u32)stva->cols; lnCol++)
								{
									// Store the name
									iSaveTo_StoreName(builder, stv);
									iAppendToTemplate(builder, "[", 1);
									iAppendToTemplateInteger(builder, lnRow);
									iAppendToTemplate(builder, ",", 1);
									iAppendToTemplateInteger(builder, lnCol);
									iAppendToTemplate(builder, "]", 1);

									// Store the data
									stv = iSaveTo_StoreData(builder, stv, false);

									// Append CR/LF after
									iAppendToTemplate(builder, "\r\n", 2);
								}
							}
						}
						// When we get here, every array has been handld

				} else {
					// Store the type and data
					stv = iSaveTo_StoreData(builder, stv, true);

					// Append CR/LF after
					iAppendToTemplate(builder, "\r\n", 2);

				}
			}


		//////////
		// Copy it back out for the caller
		/////
			lnLength = builder->currentLength;
			write_to_file(tcOutputFile, 0, 0, builder->buffer, lnLength);


		//////////
		// Release our accumulation buffer and source data buffer
		/////
			iFreeAndReleaseAccumulationTemplate(&builder, true);
			free(lcData);


		// All done
		return(lnLength);
	}

	void iSaveTo_StoreName(STemplate* builder, SSaveToVar* stv)
	{
		u32					length;
		s8*					updateAfter;
		s8*					start;
		SSaveToVarLongName*	stvln;


		if (stv->type >= 'a' && stv->type <= 'z')
		{
			// It's a lower-case letter, meaning it has a long variable name
			stvln	= (SSaveToVarLongName*)(stv+1);
			length	= stvln->length;
			start	= (s8*)(stvln+1);

		} else {
			// Regular short variable name
			start	= (s8*)&stv->name;
			length	= strlen((s8*)&stv->name);
		}

		// Store the name
		updateAfter = builder->buffer + builder->currentLength;
		iAppendToTemplate(builder, start, length);

		// Fix it up for proper capitalization
		if (length >= 2)
		{
			if (iIsNeedleInHaystack((s8*)cgcKnownFirsts, sizeof(cgcKnownFirsts) - 1, start, 1) && iIsNeedleInHaystack((s8*)cgcKnownSeconds, sizeof(cgcKnownSeconds) - 1, start + 1, 1))
			{
				// It matches the format, so we lower-case the first two characters
				iLowercase(updateAfter, 2);
				// And every character after that
				if (length >= 4)
					iLowercase(updateAfter + 3, length - 4 + 1/*base-1*/);
			}
		}
	}

	SSaveToVar* iSaveTo_StoreData(STemplate* builder, SSaveToVar* stv, bool tlStoreName)
	{
		s8*					start;
		SSaveToVarFloat*	stvf;
		SSaveToVarLogical*	stvl;
		SSaveToVarDate*		stvd;
		SSaveToVarDatetime*	stvt;
		s8					buffer[32];


		//////////
		// Store the type, length and decimals, looks like this:
		// (C:1024)
		// (L:1)
		// (N:18,7)
		//////
			// Store leading parenthesis
			iAppendToTemplate(builder, "(", 1);

			// Store the type as upper-case
			sprintf(buffer, "%c\000", stv->type);
			if (buffer[0] >= 'a' && buffer[0] <= 'z')
				buffer[0] -= 0x20;		// Convert to upper-case
			iAppendToTemplate(builder, buffer, 1);

			// Store colon between type and length data
			iAppendToTemplate(builder, ":", 1);

			// Store length data
			if (stv->type == 't' || stv->type == 'T' || stv->type == 'd' || stv->type == 'D')
			{
				// The length for these is implicit, stored as 0 explicitly.  Change that for
				// this algorithm.
				stv->length = iSwapEndian(8);
			}
			iAppendToTemplateInteger(builder, iSwapEndian(stv->length));
			if (stv->decimals != 0)
			{
				iAppendToTemplate(builder, ",", 1);
				iAppendToTemplateInteger(builder, stv->decimals);
			}

			// Store closing parenthesis
			iAppendToTemplate(builder, ")", 1);


		//////////
		// Store the name, ends up looking like this
		// (C:1024)foo = 
		//////
			// Store the variable name
			if (tlStoreName)
				iSaveTo_StoreName(builder, stv);
			iAppendToTemplate(builder, " = ", 3);


		//////////
		// Get an offset to the actual data
		//////
			start = (s8*)(stv + 1) +
						(
							(stv->type >= 'a' && stv->type <= 'z')
								? sizeof(SSaveToVarLongName) + ((SSaveToVarLongName*)(stv+1))->length 
								: 0
						);


		//////////
		// Store the actual data
		// (C:1024)foo = "whatever"
		// (N:10)foo2 = 5
		// (N:18,7)foo3 = 1.1234567
		//////
			if (stv->type == 'c' || stv->type == 'C' || stv->type == 'h' || stv->type == 'H')
			{
				// Character (h/H is HUGE character data)
				iAppendToTemplateSkipNulls(builder, (s8*)start, iSwapEndian(stv->length));
				// Indicate where the next one will go after this entry
				stv = (SSaveToVar*)(start + iSwapEndian(stv->length));

			} else if (stv->type == 'n' || stv->type == 'N') {
				// Numeric
				stvf = (SSaveToVarFloat*)start;
				iAppendToTemplateDouble(builder, stvf->fVal, iSwapEndian(stv->length), stv->decimals);
				// Indicate where the next one will go after this entry
				stv = (SSaveToVar*)(start + sizeof(SSaveToVarFloat));

			} else if (stv->type == 'l' || stv->type == 'L') {
				// Logical
				// Indicate where the next one will go after this entry
				stvl = (SSaveToVarLogical*)start;
				if (stvl->lVal == 0)
				{
					// False
					iAppendToTemplate(builder, ".F.", 3);
				} else {
					// True
					iAppendToTemplate(builder, ".T.", 3);
				}
				stv = (SSaveToVar*)(start + sizeof(SSaveToVarLogical));

			} else if (stv->type == 'd' || stv->type == 'D') {
				// Date
				stvd = (SSaveToVarDate*)start;
				// The value at stvd->dVal is a julian day number representing the date
				iComputeDateFromJulianDayNumber((u32)stvd->dVal, buffer);
				iAppendToTemplate(builder, buffer, strlen(buffer));
				// Indicate where the next one will go after this entry
				stv = (SSaveToVar*)(start + sizeof(SSaveToVarDate));

			} else if (stv->type == 't' || stv->type == 'T') {
				// Datetime
				stvt = (SSaveToVarDatetime*)start;
				// The integer portion at stvt->dVal is a julian day number representing the date
				iComputeDateFromJulianDayNumber((u32)stvt->dVal, buffer);
				iAppendToTemplate(builder, buffer, strlen(buffer));
				// The fractional number portion at stvt->dVal relates to the seconds, which backs into the time.
				iAppendToTemplate(builder, " ", 1);
				// Compute the time from the fractional part
				iComputeTimeFromFraction((f32)((stvt->dVal - (u32)stvt->dVal) * 24.0 * 60.0 * 60.0), buffer);
				iAppendToTemplate(builder, buffer, strlen(buffer));
				// Indicate where the next one will go after this entry
				stv = (SSaveToVar*)(start + sizeof(SSaveToVarDatetime));

			} else {
				// Unknown type
				iAppendToTemplate(builder, "Unknown", 1);
				// Indicate where the next one will go after this entry
				stv = (SSaveToVar*)start;
			}


		//////////
		// All done, indicate where the new pointer will be
		//////
			return(stv);
	}

	// Taken from http://stason.org/TULARC/society/calendars/2-15-1-Is-there-a-formula-for-calculating-the-Julian-day-nu.html
	// Stores: mm/dd/yyyy
	void iComputeDateFromJulianDayNumber(u32 tnJulianDayNumber, s8* tcDate11)
	{
		u32 a, b, c, d, e, m, day, month, year;

		a		= tnJulianDayNumber + 32044;
		b		= ((4 * a) + 3) / 146097;
		c		= a - ((b * 146097) / 4);
		d		= ((4 * c) + 3) / 1461;
		e		= c - ((1461 * d) / 4);
		m		= ((5 * e) + 2) / 153;
		day		= e - (((153 * m) + 2) / 5) + 1;
		month	= m + 3 - (12 * (m / 10));
		year	= (b * 100) + d - 4800 + (m / 10);
		sprintf(tcDate11, "%02u/%02u/%04u\000", month, day, year);
	}

	// Takes the number of seconds elapsed since midnight and computes time
	// hh:mm:ss.mls
	void iComputeTimeFromFraction(f32 tfSeconds, s8* tcTime13)
	{
		u32 lnHour, lnMinute, lnSecond, lnMillisecond;

		// Compute hour
		lnHour		= (u32)tfSeconds / (60 * 60);
		tfSeconds	= tfSeconds - (f32)(lnHour * 60 * 60);

		// Compute minute
		lnMinute	= (u32)tfSeconds / 60;
		tfSeconds	= tfSeconds - (f32)(lnMinute * 60);

		// Compute seconds
		lnSecond	= (u32)tfSeconds;
		tfSeconds	= tfSeconds - (f32)lnSecond;

		// Compute milliseconds
		lnMillisecond = (u32)(tfSeconds * 999.0);

		// Build the time
		sprintf(tcTime13, "%02u:%02u:%02u.%03u\000", lnHour, lnMinute, lnSecond, lnMillisecond);
	}




//////////
//
// Loads the specified file into a memory block
//
/////
	void iLoadFileContents(s8* tcInputFile, FILE** tfh, s8** tcData, u32* tnLength, u32* tnNumread)
	{
		u32		lnNumread;
		FILE*	lfh;


		// If they didn't specify parameters, we use our own
		if (!tfh)			tfh = &lfh;
		if (!tnNumread)		tnNumread = &lnNumread;

		// Initialize our "success" indicator
		*tcData = NULL;

		// Attempt to open the existing file in binary mode
		*tfh = fopen(tcInputFile, "rb+");
		if (!*tfh)
			return;

		// Find out how big it is
		fseek(*tfh, 0, SEEK_END);
		*tnLength = ftell(*tfh);
		fseek(*tfh, 0, SEEK_SET);

		// Allocate that much memory
		*tcData = (s8*)malloc(*tnLength);
		if (!*tcData)
		{
			fclose(*tfh);
			return;
		}

		// Load its contents
		*tnNumread = fread(*tcData, 1, *tnLength, *tfh);

		// If it's our file handle, close it
		if (tfh == &lfh)
			fclose(lfh);

		// If we didn't read it all, free it
		if (*tnNumread != *tnLength)
		{
			free(*tcData);
			*tcData = NULL;
		}
		// When we get here, tcData indicates success
	}




//////////
//
// Initialize the template memory area (for building output items
// through multiple steps in sequence)
//
//////
	#define _MIN_ALLOCATION_SIZE 4096
	void iAllocateAndInitializeAccumulationTemplate(STemplate** templ)
	{
		// Allocate the memory
		*templ = (STemplate*)malloc(sizeof(STemplate));
		if (*templ)
		{
			//////////
			// Initialize the memory block
			//////
				memset(*templ, 0, sizeof(STemplate));

			//////////
			// Allocate memory for our generation
			//////
				(*templ)->buffer = (s8*)malloc(_MIN_ALLOCATION_SIZE);
				if (!(*templ)->buffer)
					return;		// Failure

			//////////
			// Initialize it all to spaces, and initialize the starting buffer components
			//////
				memset((*templ)->buffer, 0, _MIN_ALLOCATION_SIZE);
				(*templ)->currentLength = 0;
				(*templ)->totSize = _MIN_ALLOCATION_SIZE;
		}
	}




//////////
//
// Appends the specified block onto the specified template, and re-allocates (enlarges)
// the destination buffer if necessary
//
//////
	void iAppendToTemplate(STemplate* dst, const s8* src, u32 length)
	{
		iAppendToTemplate(dst, (s8*)src, length);
	}

	// Appends the specified text to the destination
	void iAppendToTemplate(STemplate* dst, s8* src, u32 length)
	{
		// See if they want us to determine the length
		if (length == -1)
			length = strlen(src);

		// See if we have to re-allocate the buffer
		iAppendToTemplateValidateBufferSize(dst, length);

		// Copy the src to the dst
		memcpy(dst->buffer + dst->currentLength, src, length);
		dst->currentLength += length;
	}

	// Appends the specified number of whitespaces to the destination
	void iAppendToTemplateWhitespaces(STemplate* dst, u32 tnCount)
	{
		// See if we have to re-allocate the buffer
		iAppendToTemplateValidateBufferSize(dst, tnCount);

		// Initialize the whitespaces in the buffer
		memset(dst->buffer + dst->currentLength, ' ', tnCount);
		dst->currentLength += tnCount;
	}




//////////
//
// Appends a signed integer to the template being built
//
//////
	u32 iAppendToTemplateInteger(STemplate* dst, s32 tnValue)
	{
		u32	lnLength, lnWhitespaces;
		s8	buffer[32];


		// Convert and store
		sprintf(buffer, "%d\000", tnValue);

		// Get the overall length
		lnLength = strlen(buffer);

		// Skip past leading whitespaces
		lnWhitespaces	= 0;
		iSkipWhitespaces(buffer, &lnWhitespaces, lnLength);

		// Append only the data portion
		iAppendToTemplate(dst, buffer + lnWhitespaces, lnLength - lnWhitespaces);

		// All done
		return(lnLength);
	}




//////////
//
// Releases the previously allocated accumulation buffer template
//
//////
	void iFreeAndReleaseAccumulationTemplate(STemplate** templ, bool tbFreeBuffer)
	{
		if (*templ)
		{
			if (tbFreeBuffer && (*templ)->buffer)
			{
				// Free the buffer within
				free((*templ)->buffer);
				(*templ)->buffer = NULL;
			}

			// Free the template itself
			free(*templ);
			*templ = NULL;
		}
	}




//////////
//
// Searches through the haystack to find the needle.  Needle is NULL-
// terminated, and haystack is length-terminated.
//
//////
	bool iIsNeedleInHaystack(s8* haystack, s32 haystackLength, s8* needle, s32 needleLength)
	{
		s32 lnI;

		// Check to see if the specified word / phrase / whatever exists on this line
		for (lnI = 0; lnI <= haystackLength - needleLength; lnI++)
		{
			if (_memicmp(haystack + lnI, needle, needleLength) == 0)
				return(true);
		}

		// Failure
		return(false);
	}




//////////
//
// Lower-cases the specified string
//
//////
	void iLowercase(s8* src, u32 length)
	{
		u32 lnI;

		// Lower-case every character-by-character
		for (lnI = 0; lnI < length; lnI++)
			src[lnI] = iLowerCharacter(src[lnI]);
	}

	s8 iLowerCharacter(s8 ch)
	{
		if (ch >= 'A' && ch <= 'Z')
			ch += 0x20;
		return(ch);
	}




//////////
//
// Visual (easy-to-read) swap endian algorithm, not designed for efficiency
//
//////
	u32 iSwapEndian(u32 value)
	{
		u32 b1, b2, b3, b4;

		b1 = (value & 0xff000000) >> 24;		// Move left-most byte to right-most position
		b2 = (value & 0x00ff0000) >> 8;			// Move left-middle byte to right-middle position
		b3 = (value & 0x0000ff00) << 8;			// Move right-middle byte to left-middle position
		b4 = (value & 0x000000ff) << 24;		// Move right-most byte to left-most position

		return(b1 | b2 | b3 | b4);				// Return amalgam of them all OR'd together
	}




//////////
//
// Appends the specified block onto the specified template, and re-allocates (enlarges)
// the destination buffer if necessary, skipping nulls in the src
//
//////
	void iAppendToTemplateSkipNulls(STemplate* dst, s8* src, u32 length)
	{
		u32 lnI, lnActualLength;


		// See if they want us to determine the length
		if (length == -1)
			length = strlen(src);

		// See if we have to re-allocate the buffer
		iAppendToTemplateValidateBufferSize(dst, length);

		// Copy the src to the dst
		lnI				= 0;
		lnActualLength	= 0;
		for (lnI = 0; lnI < length; lnI++)
		{
			if (src[lnI] != 0)
			{
				*(dst->buffer + dst->currentLength + lnActualLength) = src[lnI];
				++lnActualLength;
			}
		}

		// Increase by the actual length
		dst->currentLength += lnActualLength;
	}




//////////
//
// Appends a double to the string, based on the length and decimals to store
//
//////
	u32 iAppendToTemplateDouble(STemplate* dst, f64 tfValue, u32 length, u32 decimals)
	{
		u32	lnLength, lnWhitespaces;
		s8	buffer[32];
		s8	bufferFormat[32];


		//////////
		// Store prerequisites
		//////
			// Buffer format
			sprintf(bufferFormat, "%%%u.%ulf", length, decimals);
			// Right now, buffer format looks like:
			//		%12.7lf


		//////////
		// Convert and store
		//////
			sprintf(buffer, bufferFormat, tfValue);

			// Get the overall length
			lnLength = strlen(buffer);

			// Skip past leading whitespaces
			lnWhitespaces	= 0;
			iSkipWhitespaces(buffer, &lnWhitespaces, lnLength);

			// Append only the data portion
			iAppendToTemplate(dst, buffer + lnWhitespaces, lnLength - lnWhitespaces);


		// All done
		return(lnLength);
	}




//////////
//
// Verifies the memory buffer is large enough for the space required
//
//////
	void iAppendToTemplateValidateBufferSize(STemplate* dst, u32 length)
	{
		void* dstNew;


		if (dst->currentLength + length >= dst->totSize)
		{
			// We have to resize the buffer
			dstNew = realloc(dst->buffer, dst->totSize + max(_MIN_ALLOCATION_SIZE, length * 2));
			if (!dstNew)
				return;		// Failure, no more room to allocate another 16KB chunk

			// We're good, update the structure
			dst->buffer	= (s8*)dstNew;
			dst->totSize	+= max(_MIN_ALLOCATION_SIZE, length * 2);
		}
	}




//////////
//
// Skips past whitespace characters (tabs and spaces)
// Move the offset byte forward until we're no longer sitting on a
// whitespace character, and indicate how many we skipped.
//
//////
	u32 iSkipWhitespaces(s8* source, u32* offset, u32 maxLength)
	{
		s8 c;
		u32 lnLength;

		lnLength = 0;
		while (*offset < maxLength)
		{
			c = source[*offset];
			if (c != 32/*space*/ && c != 9/*tab*/)
				return(lnLength);		// It's no longer a whitespace

			// Move to the next position
			++lnLength;
			++*offset;
		}
		return(lnLength);
	}




;
//////////
//
// Writes some data to the specified output filename
//
//////
	u32 write_to_file(s8* tcFilename, u32 tnOrigin, u32 tnOffset, s8* tcContent, u32 tnContentLength)
	{
		u32		lnNumWritten;
		FILE*	lfh;


		// Make sure there's something to do
		if (tnContentLength == 0)
			return(0);


		// Try to open an existing file
		lfh = fopen(tcFilename, "rb+");
		if (!lfh)
		{
			// File does not exist, try to create it
			lfh = fopen(tcFilename, "wb+");
			if (!lfh)
				return(-1);	// Error
		}
		// When we get here, we have our file

		// See if we're writing from a specified offset
		if (tnOrigin == 1)
		{
			// They want to begin at a particular origin
			fseek(lfh, tnOffset, SEEK_SET);
		}

		// Write the content
		lnNumWritten = fwrite(tcContent, 1, tnContentLength, lfh);
		fclose(lfh);

		// All done
		return(lnNumWritten);
	}