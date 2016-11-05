#include "data.table.h"
#include "fwriteLookups.h"
#include <errno.h>
#include <unistd.h>  // for access()
#include <fcntl.h>
#include <time.h>
#ifdef WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#define WRITE _write
#define CLOSE _close
#else
#define WRITE write
#define CLOSE close
#endif

#define NUM_SF   15
#define SIZE_SF  1000000000000000ULL  // 10^NUM_SF

// Globals for this file only (written once to hold parameters passed from R level)                   
static const char *na_str;             // by default "" or if set then usually "NA"
static size_t na_len;                  // nchar(na_str). So 0 for "", or 2 for "NA"
static char col_sep;                   // comma in .csv files
static char dec_sep;                   // the '.' in the number 3.1416. In Europe often: 3,1416
static Rboolean verbose=FALSE;         // be chatty?
static Rboolean quote=FALSE;           // whether to surround fields with double quote ". NA means 'auto' (default)
static Rboolean qmethod_escape=TRUE;   // when quoting fields, how to manage double quote in the field contents

static inline void writeInteger(long long x, char **thisCh)
{
  char *ch = *thisCh;
  // both integer and integer64 are passed to this function so careful
  // to test for NA_INTEGER in the calling code. INT_MIN (NA_INTEGER) is
  // a valid non-NA in integer64
  if (x == 0) {
    *ch++ = '0';
  } else {
    if (x<0) { *ch++ = '-'; x=-x; }
    // avoid log() call for speed. write backwards then reverse when we know how long
    int width = 0;
    while (x>0) { *ch++ = '0'+x%10; x /= 10; width++; }
    for (int i=width/2; i>0; i--) {
      char tmp=*(ch-i);
      *(ch-i) = *(ch-width+i-1);
      *(ch-width+i-1) = tmp;
    }
  }
  *thisCh = ch;
}

SEXP genLookups() {
  Rprintf("genLookups commented out of the package so it's clear it isn't needed to build. The hooks are left in so it's easy to put back in development should we need to.\n");
  // e.g. ldexpl may not be available on some platforms, or if it is it may not be accurate.
  return R_NilValue;
}
/*
  FILE *f = fopen("/tmp/fwriteLookups.h", "w"); 
  fprintf(f, "//\n\
// Generated by fwrite.c:genLookups()\n\
//\n\
// 3 vectors: sigparts, expsig and exppow\n\
// Includes precision higher than double; leave this compiler on this machine\n\
// to parse the literals at reduced precision.\n\
// 2^(-1023:1024) is held more accurately than double provides by storing its\n\
// exponent separately (expsig and exppow)\n\
// We don't want to depend on 'long double' (>64bit) availability to generate\n\
// these at runtime; libraries and hardware vary.\n\
// These small lookup tables are used for speed.\n\
//\n\n");
  fprintf(f, "double sigparts[53] = {\n0.0,\n");
  for (int i=1; i<=52; i++) {
    fprintf(f, "%.40Le%s\n",ldexpl(1.0L,-i), i==52?"":",");
  }
  fprintf(f, "};\n\ndouble expsig[2048] = {\n");
  char x[2048][60];
  for (int i=0; i<2048; i++) {
    sprintf(x[i], "%.40Le", ldexpl(1.0L, i-1023));
    fprintf(f, "%.*s%s\n", (int)(strchr(x[i],'e')-x[i]), x[i], (i==2047?"":",") );
  }
  fprintf(f, "};\n\nint exppow[2048] = {\n");
  for (int i=0; i<2048; i++) {
    fprintf(f, "%d%s", atoi(strchr(x[i],'e')+1), (i==2047?"":",") );
  }
  fprintf(f, "};\n\n");
  fclose(f);
  return R_NilValue;
}
*/

static union {
  double d;
  unsigned long long ull;
} u;

static inline void writeNumeric(double x, char **thisCh)
{
  // hand-rolled / specialized for speed
  // *thisCh is safely the output destination with enough space (ensured via calculating maxLineLen up front)
  // technique similar to base R (format.c:formatReal and printutils.c:EncodeReal0)
  // differences/tricks :
  //   i) no buffers. writes straight to the final file buffer passed to write()
  //  ii) no C libary calls such as sprintf() where the fmt string has to be interpretted over and over
  // iii) no need to return variables or flags.  Just writes.
  //  iv) shorter, easier to read and reason with. In one self contained place.
  char *ch = *thisCh;
  if (!R_FINITE(x)) {
    if (ISNAN(x)) {
      memcpy(ch, na_str, na_len); ch += na_len; // by default na_len==0 and the memcpy call will be skipped
    } else if (x>0) {
      *ch++ = 'I'; *ch++ = 'n'; *ch++ = 'f';
    } else {
      *ch++ = '-'; *ch++ = 'I'; *ch++ = 'n'; *ch++ = 'f';
    }
  } else if (x == 0.0) {
    *ch++ = '0';   // and we're done.  so much easier rather than passing back special cases
  } else {
    if (x < 0.0) { *ch++ = '-'; x = -x; }  // and we're done on sign, already written. no need to pass back sign
    u.d = x;
    unsigned long long fraction = u.ull & 0xFFFFFFFFFFFFF;  // (1ULL<<52)-1;
    int exponent = (int)((u.ull>>52) & 0x7FF);              // [0,2047]

    // Now sum the appropriate powers 2^-(1:52) of the fraction 
    // Important for accuracy to start with the smallest first; i.e. 2^-52
    // Exact powers of 2 (1.0, 2.0, 4.0, etc) are represented precisely with fraction==0
    // Skip over tailing zeros for exactly representable numbers such 0.5, 0.75
    // Underflow here (0u-1u = all 1s) is on an unsigned type which is ok by C standards
    // sigparts[0] arranged to be 0.0 in genLookups() to enable branch free loop here
    double acc = 0;  // 'long double' not needed
    int i = 52;
    if (fraction) {
      while ((fraction & 0xFF) == 0) { fraction >>= 8; i-=8; } 
      while (fraction) {
        acc += sigparts[(((fraction&1u)^1u)-1u) & i];
        i--;
        fraction >>= 1;
      }
    }
    // 1.0+acc is in range [1.5,2.0) by IEEE754
    // expsig is in range [1.0,10.0) by design of fwriteLookups.h
    // Therefore y in range [1.5,20.0)
    // Avoids (potentially inaccurate and potentially slow) log10/log10l, pow/powl, ldexp/ldexpl
    // By design we can just lookup the power from the tables
    double y = (1.0+acc) * expsig[exponent];  // low magnitude mult
    int exp = exppow[exponent];
    if (y>=10.0) { y /= 10; exp++; }
    unsigned long long l = y * SIZE_SF;  // low magnitude mult 10^NUM_SF
    // l now contains NUM_SF+1 digits as integer where repeated /10 below is accurate

    // if (verbose) Rprintf("\nTRACE: acc=%.20Le ; y=%.20Le ; l=%llu ; e=%d     ", acc, y, l, exp);    

    if (l%10 >= 5) l+=10; // use the last digit to round
    l /= 10;
    if (l == 0) {
      if (*(ch-1)=='-') ch--;
      *ch++ = '0';
    } else {
      // Count trailing zeros and therefore s.f. present in l
      int trailZero = 0;
      while (l%10 == 0) { l /= 10; trailZero++; }
      int sf = NUM_SF - trailZero;
      if (sf==0) {sf=1; exp++;}  // e.g. l was 9999999[5-9] rounded to 10000000 which added 1 digit
      
      // l is now an unsigned long that doesn't start or end with 0
      // sf is the number of digits now in l
      // exp is e<exp> were l to be written with the decimal sep after the first digit
      int dr = sf-exp-1; // how many characters to print to the right of the decimal place
      int width=0;       // field width were it written decimal format. Used to decide whether to or not.
      int dl0=0;         // how many 0's to add to the left of the decimal place before starting l
      if (dr<=0) { dl0=-dr; dr=0; width=sf+dl0; }  // 1, 10, 100, 99000
      else {
        if (sf>dr) width=sf+1;                     // 1.234 and 123.4
        else { dl0=1; width=dr+1+dl0; }            // 0.1234, 0.0001234
      }
      // So:  3.1416 => l=31416, sf=5, exp=0     dr=4; dl0=0; width=6
      //      30460  => l=3046, sf=4, exp=4      dr=0; dl0=1; width=5
      //      0.0072 => l=72, sf=2, exp=-3       dr=4; dl0=1; width=6
      if (width <= sf + (sf>1) + 2 + (abs(exp)>99?3:2)) {
         //              ^^^^ to not include 1 char for dec in -7e-04 where sf==1
         //                      ^ 2 for 'e+'/'e-'
         // decimal format ...
         ch += width-1;
         if (dr) {
           while (dr && sf) { *ch--='0'+l%10; l/=10; dr--; sf--; }
           while (dr) { *ch--='0'; dr--; }
           *ch-- = dec_sep;
         }
         while (dl0) { *ch--='0'; dl0--; }
         while (sf) { *ch--='0'+l%10; l/=10; sf--; }
         // ch is now 1 before the first char of the field so position it afterward again, and done
         ch += width+1;
      } else {
        // scientific ...
        ch += sf;  // sf-1 + 1 for dec
        for (int i=sf; i>1; i--) {
          *ch-- = '0' + l%10;   
          l /= 10;
        }
        if (sf == 1) ch--; else *ch-- = dec_sep;
        *ch = '0' + l;
        ch += sf + (sf>1);
        *ch++ = 'e';  // lower case e to match base::write.csv
        if (exp < 0) { *ch++ = '-'; exp=-exp; }
        else { *ch++ = '+'; }  // to match base::write.csv
        if (exp < 100) {
          *ch++ = '0' + (exp / 10);
          *ch++ = '0' + (exp % 10);
        } else {
          *ch++ = '0' + (exp / 100);
          *ch++ = '0' + (exp / 10) % 10;
          *ch++ = '0' + (exp % 10);
        }
      }
    }
  }
  *thisCh = ch;
}

static inline void writeString(SEXP x, char **thisCh)
{
  char *ch = *thisCh;
  if (x == NA_STRING) {
    // NA is not quoted by write.csv even when quote=TRUE to distinguish from "NA"
    memcpy(ch, na_str, na_len); ch += na_len;
  } else {
    Rboolean q = quote;
    if (q==NA_LOGICAL) { // quote="auto"
      const char *tt = CHAR(x);
      while (*tt!='\0' && *tt!=col_sep && *tt!='\n') *ch++ = *tt++;
      // windows includes \n in its \r\n so looking for \n only is sufficient
      if (*tt=='\0') {
        // most common case: no sep or newline contained in string
        *thisCh = ch;  // advance caller over the field already written
        return;
      }
      ch = *thisCh; // rewind the field written since it contains some sep or \n
      q = TRUE;
    }
    if (q==FALSE) {
      memcpy(ch, CHAR(x), LENGTH(x));
      ch += LENGTH(x);
    } else {
      *ch++ = '"';
      const char *tt = CHAR(x);
      if (qmethod_escape) {
        while (*tt!='\0') {
          if (*tt=='"' || *tt=='\\') *ch++ = '\\';
          *ch++ = *tt++;
        }
      } else {
        // qmethod='double'
        while (*tt!='\0') {
          if (*tt=='"') *ch++ = '"';
          *ch++ = *tt++;
        }
      }
      *ch++ = '"';
    }
  }
  *thisCh = ch;
}

static inline Rboolean isInteger64(SEXP x) {
  SEXP class;
  if (TYPEOF(x)==REALSXP && isString(class = getAttrib(x, R_ClassSymbol))) {
    for (int i=0; i<LENGTH(class); i++) {   // inherits()
      if (STRING_ELT(class, i) == char_integer64) return TRUE;
    }
  }
  return FALSE;
}

SEXP writefile(SEXP DF,                 // any list of same length vectors; e.g. data.frame, data.table
               SEXP filenameArg,
               SEXP col_sep_Arg,
               SEXP row_sep_Arg,
               SEXP na_Arg,
               SEXP dec_Arg,
               SEXP quoteArg,           // 'auto'=NA_LOGICAL|TRUE|FALSE
               SEXP qmethod_escapeArg,  // TRUE|FALSE
               SEXP append,             // TRUE|FALSE
               SEXP row_names,          // TRUE|FALSE
               SEXP col_names,          // TRUE|FALSE
               SEXP logicalAsInt_Arg,   // TRUE|FALSE
               SEXP buffMB_Arg,         // [1-1024] default 8MB
               SEXP nThread,
               SEXP showProgressArg,
               SEXP verboseArg,
               SEXP turboArg)
{
  if (!isNewList(DF)) error("fwrite must be passed an object of type list; e.g. data.frame, data.table");
  RLEN ncol = length(DF);
  if (ncol==0) error("fwrite must be passed a non-empty list");
  RLEN nrow = length(VECTOR_ELT(DF, 0));
  for (int i=1; i<ncol; i++) {
    if (nrow != length(VECTOR_ELT(DF, i)))
      error("Column %d's length (%d) is not the same as column 1's length (%d)", i+1, length(VECTOR_ELT(DF, i)), nrow);
  }
#ifndef _OPENMP
  Rprintf("This installation has no OpenMP support. fwrite() will still work but slower in single threaded mode.\n");
  // Not warning() because that would cause test.data.table() to error about the unexpected warnings
#endif

  const Rboolean showProgress = LOGICAL(showProgressArg)[0];
  time_t start_time = time(NULL);
  time_t next_time = start_time+2; // start printing progress meter in 2 sec if not completed by then
  
  verbose = LOGICAL(verboseArg)[0];
  const Rboolean turbo = LOGICAL(turboArg)[0];
  
  col_sep = *CHAR(STRING_ELT(col_sep_Arg, 0));  // DO NOT DO: allow multichar separator (bad idea)
  const char *row_sep = CHAR(STRING_ELT(row_sep_Arg, 0));
  int row_sep_len = strlen(row_sep);  // someone somewhere might want a trailer on every line
  na_str = CHAR(STRING_ELT(na_Arg, 0));
  na_len = strlen(na_str);
  dec_sep = *CHAR(STRING_ELT(dec_Arg,0));
  quote = LOGICAL(quoteArg)[0];
  qmethod_escape = LOGICAL(qmethod_escapeArg)[0];
  const char *filename = CHAR(STRING_ELT(filenameArg, 0));
  Rboolean logicalAsInt = LOGICAL(logicalAsInt_Arg)[0];
  int nth = INTEGER(nThread)[0];

  int f;
  if (*filename=='\0') {
    f=-1;  // file="" means write to standard output
    row_sep = "\n";  // We'll use Rprintf(); it knows itself about \r\n on Windows
    row_sep_len = 1;
  } else { 
#ifdef WIN32
    f = _open(filename, _O_WRONLY | _O_BINARY | _O_CREAT | (LOGICAL(append)[0] ? _O_APPEND : _O_TRUNC), _S_IWRITE);
    // row_sep must be passed from R level as '\r\n' on Windows since write() only auto-converts \n to \r\n in
    // _O_TEXT mode. We use O_BINARY for full control and perhaps speed since O_TEXT must have to deep branch an if('\n')
#else
    f = open(filename, O_WRONLY | O_CREAT | (LOGICAL(append)[0] ? O_APPEND : O_TRUNC), 0644);
#endif
    if (f == -1) {
      char *err = strerror(errno);
      if( access( filename, F_OK ) != -1 )
        error("%s: '%s'. Failed to open existing file for writing. Do you have write permission to it? Is this Windows and does another process such as Excel have it open?", err, filename);
      else
        error("%s: '%s'. Unable to create new file for writing (it does not exist already). Do you have permission to write here, is there space on the disk and does the path exist?", err, filename); 
    }
  }
  clock_t t0=clock();
  
  // Store column type tests in lookups for efficiency
  int sameType = TYPEOF(VECTOR_ELT(DF, 0)); // to avoid deep switch later
  SEXP levels[ncol];        // VLA. NULL means not-factor too, else the levels
  Rboolean integer64[ncol]; // VLA
  for (int j=0; j<ncol; j++) {
    SEXP column = VECTOR_ELT(DF, j);
    levels[j] = NULL;
    integer64[j] = FALSE;
    if (isFactor(column)) {
      levels[j] = getAttrib(column, R_LevelsSymbol);
      sameType = 0;
    } else if (isInteger64(column)) {  // tests class() string vector
      integer64[j] = TRUE;
      sameType = 0;
    } else {
      if (TYPEOF(column) != sameType) sameType = 0;
    }
  }
  
  // user may want row names even when they don't exist (implied row numbers as row names)
  Rboolean doRowNames = LOGICAL(row_names)[0];
  SEXP rowNames = NULL;
  if (doRowNames) {
    rowNames = getAttrib(DF, R_RowNamesSymbol);
    if (!isString(rowNames)) rowNames=NULL;
  }
  
  // Estimate max line length of a 1000 row sample (100 rows in 10 places).
  // Estimate even of this sample because quote='auto' may add quotes and escape embedded quotes.
  // Buffers will be resized later if there are too many actual line lengths outside the sample estimate.
  int maxLineLen = 0;
  int step = nrow<1000 ? 100 : nrow/10;
  for (int start=0; start<nrow; start+=step) {
    int end = (nrow-start)<100 ? nrow : start+100;
    for (int i=start; i<end; i++) {
      int thisLineLen=0;
      if (doRowNames) {
        if (rowNames) thisLineLen += LENGTH(STRING_ELT(rowNames,i));
        else thisLineLen += 1+(int)log10(nrow);
        if (quote==TRUE) thisLineLen+=2;
        thisLineLen++; // col_sep
      }
      for (int j=0; j<ncol; j++) {
        SEXP column = VECTOR_ELT(DF, j);
        static char tmp[32]; // +- 15digits dec e +- nnn \0 = 23 + 9 safety = 32. Covers integer64 too (20 digits).
        char *ch=tmp;
        switch(TYPEOF(column)) {
        case LGLSXP:
          thisLineLen = logicalAsInt ? 1/*0|1*/ : 5/*FALSE*/;  // na_len might be 2 (>1) but ok; this is estimate
          break;
        case INTSXP: {
          int i32 = INTEGER(column)[i];
          if (i32 == NA_INTEGER) thisLineLen += na_len;
          else if (levels[j] != NULL) thisLineLen += LENGTH(STRING_ELT(levels[j], i32-1));
          else { writeInteger(i32, &ch); thisLineLen += (int)(ch-tmp); } }
          break;          
        case REALSXP:
          if (integer64[j]) {
            long long i64 = *(long long *)&REAL(column)[i];
            if (i64==NAINT64)                              thisLineLen += na_len;
            else { writeInteger(i64,             &ch); thisLineLen += (int)(ch-tmp); }
          } else { writeNumeric(REAL(column)[i], &ch); thisLineLen += (int)(ch-tmp); }
          break;
        case STRSXP:
          thisLineLen += LENGTH(STRING_ELT(column, i));
          break;
        default:
          error("Column %d's type is '%s' - not yet implemented.", j+1, type2char(TYPEOF(column)) );
        }
        thisLineLen++; // col_sep
      }
      thisLineLen += row_sep_len;
      if (thisLineLen > maxLineLen) maxLineLen = thisLineLen;
    }
  }
  if (verbose) Rprintf("maxLineLen=%d from sample. Found in %.3fs\n", maxLineLen, 1.0*(clock()-t0)/CLOCKS_PER_SEC);
  
  t0=clock();  
  if (verbose) {
    Rprintf("Writing column names ... ");
    if (f==-1) Rprintf("\n");
  }
  if (LOGICAL(col_names)[0]) {
    SEXP names = getAttrib(DF, R_NamesSymbol);  
    if (names!=NULL) {
      if (LENGTH(names) != ncol) error("Internal error: length of column names is not equal to the number of columns. Please report.");
      // allow for quoting even when not.
      int buffSize = 2/*""*/ +1/*,*/;
      for (int j=0; j<ncol; j++) buffSize += 1/*"*/ +2*LENGTH(STRING_ELT(names, j)) +1/*"*/ +1/*,*/;
      //     in case every name full of quotes(!) to be escaped ^^
      buffSize += row_sep_len +1/*\0*/;
      char *buffer = malloc(buffSize);
      if (buffer == NULL) error("Unable to allocate %d buffer for column names", buffSize);
      char *ch = buffer;
      if (doRowNames) {
        if (quote!=FALSE) { *ch++='"'; *ch++='"'; } // to match write.csv
        *ch++ = col_sep;
      }
      for (int j=0; j<ncol; j++) {
        writeString(STRING_ELT(names, j), &ch);
        *ch++ = col_sep;
      }
      ch--;  // backup onto the last col_sep after the last column
      memcpy(ch, row_sep, row_sep_len);  // replace it with the newline 
      ch += row_sep_len;
      if (f==-1) { *ch='\0'; Rprintf(buffer); }
      else if (WRITE(f, buffer, (int)(ch-buffer))==-1) {
        int errwrite=errno;
        close(f); // the close might fail too but we want to report the write error
        free(buffer);
        error("%s: '%s'", strerror(errwrite), filename);
      }
      free(buffer);
    }
  }
  if (verbose) Rprintf("done in %.3fs\n", 1.0*(clock()-t0)/CLOCKS_PER_SEC);
  if (nrow == 0) {
    if (verbose) Rprintf("No data rows present (nrow==0)\n");
    if (f!=-1 && CLOSE(f)) error("%s: '%s'", strerror(errno), filename);
    return(R_NilValue);
  }

  // Decide buffer size and rowsPerBatch for each thread
  // Once rowsPerBatch is decided it can't be changed, but we can increase buffer size if the lines
  // turn out to be longer than estimated from the sample.
  // buffSize large enough to fit many lines to i) reduce calls to write() and ii) reduce thread sync points
  // It doesn't need to be small in cache because it's written contiguously.
  // If we don't use all the buffer for any reasons that's ok as OS will only page in the pages touched.
  // So, generally the larger the better up to max filesize/nth to use all the threads. A few times
  //   smaller than that though, to achieve some load balancing across threads since schedule(dynamic).
  int buffMB = INTEGER(buffMB_Arg)[0]; // checked at R level between 1 and 1024
  if (buffMB<1 || buffMB>1024) error("buffMB=%d outside [1,1024]", buffMB); // check it again even so
  int buffSize = 1024*1024*buffMB;
  int rowsPerBatch =
    (10*maxLineLen > buffSize) ? 1 :  // very long lines > 100,000 characters if buffMB==1
    0.9 * buffSize/maxLineLen;        // 10% overage
  if (rowsPerBatch > nrow) rowsPerBatch=nrow;
  int numBatches = (nrow-1)/rowsPerBatch + 1;
  if (numBatches < nth) nth = numBatches;
  if (verbose) {
    Rprintf("Writing %d rows in %d batches of %d rows (each buffer size %dMB, turbo=%d, showProgress=%d, nth=%d) ... ",
    nrow, numBatches, rowsPerBatch, buffMB, turbo, showProgress, nth);
    if (f==-1) Rprintf("\n");
  }
  t0 = clock();
  
  Rboolean failed=FALSE, hasPrinted=FALSE;
  int failed_reason=0;  // -1 for malloc fail, else write's errno (>=1)
  #pragma omp parallel num_threads(nth)
  {
    char *ch, *buffer;              // local to each thread
    ch = buffer = malloc(buffSize);  // each thread has its own buffer
    // Don't use any R API alloc here (e.g. R_alloc); they are
    // not thread-safe as per last sentence of R-exts 6.1.1. 
    
    if (buffer==NULL) {failed=TRUE; failed_reason=-1;}
    // Do not rely on availability of '#omp cancel' new in OpenMP v4.0 (July 2013).
    // OpenMP v4.0 is in gcc 4.9+ (https://gcc.gnu.org/wiki/openmp) but
    // not yet in clang as of v3.8 (http://openmp.llvm.org/)
    // If not-me failed, I'll see shared 'failed', fall through loop, free my buffer
    // and after parallel section, single thread will call R API error() safely.
    
    #pragma omp single
    {
      nth = omp_get_num_threads();  // update nth with the actual nth (might be different than requested)
    }
    int me = omp_get_thread_num();
    
    #pragma omp for ordered schedule(dynamic)
    for(RLEN start=0; start<nrow; start+=rowsPerBatch) {
      if (failed) continue;  // Not break. See comments above about #omp cancel
      int end = ((nrow-start)<rowsPerBatch) ? nrow : start+rowsPerBatch;
      
      // all-integer and all-double deep switch() avoidance. We could code up all-integer64
      // as well but that seems even less likely in practice than all-integer or all-double
      if (turbo && sameType==REALSXP && !doRowNames) {
        // avoid deep switch() on type. turbo switches on both sameType and specialized writeNumeric
        for (RLEN i=start; i<end; i++) {
          for (int j=0; j<ncol; j++) {
            SEXP column = VECTOR_ELT(DF, j);
            writeNumeric(REAL(column)[i], &ch);
            *ch++ = col_sep;
          }
          ch--;  // backup onto the last col_sep after the last column
          memcpy(ch, row_sep, row_sep_len);  // replace it with the newline.
          ch += row_sep_len;
        }
      } else if (turbo && sameType==INTSXP && !doRowNames) {
        for (RLEN i=start; i<end; i++) {
          for (int j=0; j<ncol; j++) {
            SEXP column = VECTOR_ELT(DF, j);
            if (INTEGER(column)[i] == NA_INTEGER) {
              memcpy(ch, na_str, na_len); ch += na_len;
            } else {
              writeInteger(INTEGER(column)[i], &ch);
            }
            *ch++ = col_sep;
          }
          ch--;
          memcpy(ch, row_sep, row_sep_len);
          ch += row_sep_len;
        }
      } else {
        // mixed types. switch() on every cell value since must write row-by-row
        for (RLEN i=start; i<end; i++) {
          if (doRowNames) {
            if (rowNames==NULL) {
              if (quote!=FALSE) *ch++='"';  // default 'auto' will quote the row.name numbers
              writeInteger(i+1, &ch);
              if (quote!=FALSE) *ch++='"';
            } else {
              writeString(STRING_ELT(rowNames, i), &ch);
            }
            *ch++=col_sep;
          }
          for (int j=0; j<ncol; j++) {
            SEXP column = VECTOR_ELT(DF, j);
            switch(TYPEOF(column)) {
            case LGLSXP: {
              Rboolean bool = LOGICAL(column)[i];
              if (bool == NA_LOGICAL) {
                memcpy(ch, na_str, na_len); ch += na_len;
              } else if (logicalAsInt) {
                *ch++ = '0'+bool;
              } else if (bool) {
                *ch++='T'; *ch++='R'; *ch++='U'; *ch++='E';
              } else {
                *ch++='F'; *ch++='A'; *ch++='L'; *ch++='S'; *ch++='E';
              } }
              break;
            case REALSXP:
              if (integer64[j]) {
                long long i64 = *(long long *)&REAL(column)[i];
                if (i64 == NAINT64) {
                  memcpy(ch, na_str, na_len); ch += na_len;
                } else {
                  if (turbo) {
                    writeInteger(i64, &ch);
                  } else {
                    ch += sprintf(ch,
                    #ifdef WIN32
                        "%I64d"
                    #else
                        "%lld"
                    #endif
                    , i64);
                  }
                }
              } else {
                if (turbo) {
                  writeNumeric(REAL(column)[i], &ch); // handles NA, Inf etc within it
                } else {
                  // if there are any problems with the specialized writeNumeric, user can revert to (slower) standard library
                  if (ISNAN(REAL(column)[i])) {
                    memcpy(ch, na_str, na_len); ch += na_len;
                  } else {
                    ch += sprintf(ch, "%.15g", REAL(column)[i]);
                  }
                }
              }
              break;
            case INTSXP:
              if (INTEGER(column)[i] == NA_INTEGER) {
                memcpy(ch, na_str, na_len); ch += na_len;
              } else if (levels[j] != NULL) {   // isFactor(column) == TRUE
                writeString(STRING_ELT(levels[j], INTEGER(column)[i]-1), &ch);
              } else {
                if (turbo) {
                  writeInteger(INTEGER(column)[i], &ch);
                } else {
                  ch += sprintf(ch, "%d", INTEGER(column)[i]);
                }
              }
              break;
            case STRSXP:
              writeString(STRING_ELT(column, i), &ch);
              break;
            // default:
            // An uncovered type would have already thrown above when calculating maxLineLen earlier
            }
            *ch++ = col_sep;
          }
          ch--;  // backup onto the last col_sep after the last column
          memcpy(ch, row_sep, row_sep_len);  // replace it with the newline. TODO: replace memcpy call with eol1 eol2 --eolLen 
          ch += row_sep_len;
        }
      }
      #pragma omp ordered
      {
        if (!failed) { // a thread ahead of me could have failed below while I was working or waiting above
          if (f==-1) {
            *ch='\0';  // standard C string end marker so Rprintf knows where to stop
            Rprintf(buffer);
            // nth==1 at this point since when file=="" (f==-1 here) fwrite.R calls setDTthreads(1)
            // Although this ordered section is one-at-a-time it seems that calling Rprintf() here, even with a
            // R_FlushConsole() too, causes corruptions on Windows but not on Linux. At least, as observed so
            // far using capture.output(). Perhaps Rprintf() updates some state or allocation that cannot be done
            // by slave threads, even when one-at-a-time. Anyway, made this single-threaded when output to console
            // to be safe (setDTthreads(1) in fwrite.R) since output to console doesn't need to be fast.
          } else {
            if (WRITE(f, buffer, (int)(ch-buffer)) == -1) {
              failed=TRUE; failed_reason=errno;
            }
            time_t now;
            if (me==0 && showProgress && (now=time(NULL))>=next_time && !failed) {
              // See comments above inside the f==-1 clause.
              // Not only is this ordered section one-at-a-time but we'll also Rprintf() here only from the
              // master thread (me==0) and hopefully this will work on Windows. If not, user should set
              // showProgress=FALSE until this can be fixed or removed.
              int eta = (int)((nrow-end)*(((double)(now-start_time))/end));
              if (hasPrinted || eta >= 2) {
                if (verbose && !hasPrinted) Rprintf("\n"); 
                Rprintf("\rWritten %.1f%% of %d rows in %d secs using %d thread%s. ETA %d secs.    ",
                         (100.0*end)/nrow, nrow, (int)(now-start_time), nth, nth==1?"":"s", eta);
                R_FlushConsole();    // for Windows
                next_time = now+1;
                hasPrinted = TRUE;
              }
            }
            // May be possible for master thread (me==0) to call R_CheckUserInterrupt() here.
            // Something like: 
            // if (me==0) {
            //   failed = TRUE;  // inside ordered here; the slaves are before ordered and not looking at 'failed'
            //   R_CheckUserInterrupt();
            //   failed = FALSE; // no user interrupt so return state
            // }
            // But I fear the slaves will hang waiting for the master (me==0) to complete the ordered
            // section which may not happen if the master thread has been interrupted. Rather than
            // seeing failed=TRUE and falling through to free() and close() as intended.
            // Could register a finalizer to free() and close() perhaps :
            // http://r.789695.n4.nabble.com/checking-user-interrupts-in-C-code-tp2717528p2717722.html
            // Conclusion for now: do not provide ability to interrupt.
            // write() errors and malloc() fails will be caught and cleaned up properly, however.
          }
          ch = buffer;  // back to the start of me's buffer ready to fill it up again
        }
      }
    }
    free(buffer);
    // all threads will call this free on their buffer, even if one or more threads had malloc fail.
    // If malloc() failed for me, free(NULL) is ok and does nothing.
  }
  // Finished parallel region and can call R API safely now.
  if (hasPrinted) {
    // clear the progress meter
    Rprintf("\r                                                                                   \r");
    R_FlushConsole();  // for Windows
  }
  if (f!=-1 && CLOSE(f) && !failed)
    error("%s: '%s'", strerror(errno), filename);
  // quoted '%s' in case of trailing spaces in the filename
  // If a write failed, the line above tries close() to clean up, but that might fail as well. The
  // && !failed is to not report the error as just 'closing file' but the next line for more detail
  // from the original error on write.
  if (failed) {
    if (failed_reason==-1) {
      error("One or more threads failed to alloc or realloc their private buffer. Out of memory.\n");
    } else {
      error("%s: '%s'", strerror(failed_reason), filename);
    }
  }
  if (verbose) Rprintf("done (actual nth=%d)\n", nth);
  return(R_NilValue);
}


