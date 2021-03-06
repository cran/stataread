/**
  Read  Stata version 6.0 and 5.0 .dta files, write version 6.0.
  
  (c) 1999, 2000 Thomas Lumley. 

  The format of Stata files is documented under 'file formats' 
  in the Stata manual.

  This code currently does not make use of the print format or value
  label information in a .dta file. It cannot handle files with 'int'
  'float' or 'double' that differ from IEEE 4-byte integer, 4-byte
  real and 8-byte real respectively: it's not clear whether such files
  can exist.

  Versions of Stata before 4.0 used different file formats.

**/


#include "R.h"
#include "Rinternals.h"
#include <stdio.h>

/** R 1.2 compatibility definitions **/
#if R_VERSION < R_Version(1, 2, 0)
# define STRING_ELT(x,i)    (STRING(x)[i])
# define VECTOR_ELT(x,i)    (VECTOR(x)[i])
# define SET_STRING_ELT(x,i,v)  (STRING(x)[i]=(v))
# define SET_VECTOR_ELT(x,i,v)  (VECTOR(x)[i]=(v))
#endif

/* handling endianess*/
#define LOHI 1
#define HILO 2
#define NATIVE_ENDIAN 0

/* Stata format constants */
#define STATA_FLOAT  'f'
#define STATA_DOUBLE 'd'
#define STATA_INT    'l'
#define STATA_SHORTINT 'i'
#define STATA_BYTE  'b'

#define STATA_STRINGOFFSET 0x7f

#define STATA_BYTE_NA 127
#define STATA_SHORTINT_NA 32767
#define STATA_INT_NA 2147483647

static double STATA_FLOAT_NA;
static double STATA_DOUBLE_NA;
static int endian,stata_endian;


/** these are not portable, but we can only handle Stata files
    on machines with IEEE numbers anyway
**/
typedef union {
  double value;
  unsigned char bytes[8];
} stata8;

typedef union {
  int ivalue;
  float fvalue;
  unsigned char bytes[4];
} stata4;

static double swapd(double d){
    stata8 rval,input;
    input.value=d;
    rval.bytes[7]=input.bytes[0];
    rval.bytes[0]=input.bytes[7];
    rval.bytes[6]=input.bytes[1];
    rval.bytes[1]=input.bytes[6];
    rval.bytes[5]=input.bytes[2];
    rval.bytes[2]=input.bytes[5];
    rval.bytes[4]=input.bytes[3];
    rval.bytes[3]=input.bytes[4];
    return rval.value;
}


static int swapi(int i){
    stata4 rval,input;
    input.ivalue=i;
    rval.bytes[0]=input.bytes[3];
    rval.bytes[3]=input.bytes[0];
    rval.bytes[1]=input.bytes[2];
    rval.bytes[2]=input.bytes[1];
    return rval.ivalue;
}


static double swapf(float f){
    stata4 rval,input;
    input.fvalue=f;
    rval.bytes[0]=input.bytes[3];
    rval.bytes[3]=input.bytes[0];
    rval.bytes[1]=input.bytes[2];
    rval.bytes[2]=input.bytes[1];
    return (double) rval.fvalue;
}




typedef union
{
  double value;
  unsigned int word[2];
} ieee_double;


static void setup_consts()
{
    ieee_double x;
    x.value = 1;
    if (x.word[0] == 0x3ff00000) {
	endian=LOHI;
    }
    else if (x.word[1] == 0x3ff00000) {
	endian=HILO;
    }
    else error("couldn't determine endianness.");

    STATA_FLOAT_NA=pow(2,127);
    STATA_DOUBLE_NA=pow(2,1023);
      
}


/** Low-level input **/

static int InIntegerBinary(FILE * fp, int naok, int swapends)
{
    int i;
    if (fread(&i, sizeof(int), 1, fp) != 1)
	error("a binary read error occured");
    if (swapends)
	i=swapi(i);
    return ((i==STATA_INT_NA) & !naok ? NA_INTEGER : i);
}

static int InByteBinary(FILE * fp, int naok)
{ 
    unsigned char i;
    if (fread(&i, sizeof(char), 1, fp) != 1)
	error("a binary read error occured");
    return  ((i==STATA_BYTE_NA) & !naok ? NA_INTEGER : (int) i);
}

static int InShortIntBinary(FILE * fp, int naok,int swapends)
{
  unsigned short first,second,result;
  
  first = InByteBinary(fp,1);
  second = InByteBinary(fp,1);
  if (stata_endian==LOHI){
    result= (first<<8) | second;
  } else {
    result= (second<<8) | first;
  }
  return ((result==STATA_SHORTINT_NA) & !naok ? NA_INTEGER  : result);
}


static double InDoubleBinary(FILE * fp, int naok, int swapends)
{
    double i;
    if (fread(&i, sizeof(double), 1, fp) != 1)
	error("a binary read error occured");
    if (swapends)
	i=swapd(i);
    return ((i==STATA_DOUBLE_NA) & !naok ? NA_REAL : i);
}

static float InFloatBinary(FILE * fp, int naok, int swapends)
{
    float i;
    if (fread(&i, sizeof(float), 1, fp) != 1)
	error("a binary read error occured");
    if (swapends)
	i= swapf(i);
    return ((i==STATA_FLOAT_NA) & !naok ? (float) NA_REAL :  i);
}

static void InStringBinary(FILE * fp, int nchar, char* buffer)
{
    if (fread(buffer, nchar, 1, fp) != 1)
	error("a binary read error occured");
}

static char* nameMangle(char *stataname, int len){
    int i;
    for(i=0;i<len;i++)
      if (stataname[i]=='_') stataname[i]='.';
    return stataname;
}


/*****
      Turn a .dta file into a data frame
      Variable labels go to attributes of the data frame

      value labels and characteristics could go as attributes of the variables 
      not yet implemented
****/



SEXP R_LoadStataData(FILE *fp)
{
    int i,j,nvar,nobs,charlen, version5,swapends;
    unsigned char abyte;
    char datalabel[81], timestamp[18], aname[9];
    SEXP df,names,tmp,varlabels,types,row_names;
   
    
    setup_consts();  /*endianness*/

    /** first read the header **/
    
    abyte=InByteBinary(fp,1);   /* release version */
    version5=0;  /*-Wall*/
    switch (abyte){
    case 0x69:
        version5=1;
	break;
    case 'l':
        version5=0;
	break;
    default:
        error("Not a Stata v5 or v6 file");
    }
    stata_endian=(int) InByteBinary(fp,1);     /* byte ordering */
    if (endian!=stata_endian)
	swapends=1;
    else 
	swapends=0;

    InByteBinary(fp,1);            /* filetype -- junk */
    InByteBinary(fp,1);            /* padding */
    nvar =  (InShortIntBinary(fp,1,swapends)); /* number of variables */
    nobs =(InIntegerBinary(fp,1,swapends));  /* number of cases */
    /* data label - zero terminated string */
    if (version5)         
        InStringBinary(fp,32,datalabel);
    else
        InStringBinary(fp,81,datalabel);   
    /* file creation time - zero terminated string */
    InStringBinary(fp,18,timestamp);  
  
    /** make the data frame **/

    PROTECT(df=allocVector(VECSXP, nvar));
   
    /** and now stick the labels on it **/
    
    PROTECT(tmp=allocVector(STRSXP,1));
    /* STRING(tmp)[0]=mkChar(datalabel);*/
    SET_STRING_ELT(tmp,0,mkChar(datalabel));
    setAttrib(df,install("datalabel"),tmp);
    UNPROTECT(1);
    PROTECT(tmp=allocVector(STRSXP,1));
    /* STRING(tmp)[0]=mkChar(timestamp);*/
    SET_STRING_ELT(tmp,0,mkChar(timestamp));
    setAttrib(df,install("time.stamp"),tmp);
    UNPROTECT(1);

      
    /** read variable descriptors **/
    
    /** types **/
    
    PROTECT(types=allocVector(INTSXP,nvar));
    for(i=0;i<nvar;i++){
        abyte = InByteBinary(fp,1);
	INTEGER(types)[i]= abyte;
        switch (abyte) {
	case STATA_FLOAT:
	case STATA_DOUBLE:
	    /* VECTOR(df)[i]=allocVector(REALSXP,nobs);*/
	    SET_VECTOR_ELT(df,i,allocVector(REALSXP,nobs));
	    break;
	case STATA_INT:
	case STATA_SHORTINT:
	case STATA_BYTE:
	    /* VECTOR(df)[i]=allocVector(INTSXP,nobs);*/
	    SET_VECTOR_ELT(df,i,allocVector(INTSXP,nobs));
	    break;
	default:
	    if (abyte<STATA_STRINGOFFSET)
	      error("Unknown data type");
	    /* VECTOR(df)[i]=allocVector(STRSXP,nobs);*/
	    SET_VECTOR_ELT(df,i,allocVector(STRSXP,nobs));
	    break;
	}
    }

    /** names **/

    PROTECT(names=allocVector(STRSXP,nvar));
    for (i=0;i<nvar;i++){
        InStringBinary(fp,9,aname);
        /* STRING(names)[i]=mkChar(nameMangle(aname,9));*/
	SET_STRING_ELT(names,i,mkChar(nameMangle(aname,9)));
    }
    setAttrib(df,R_NamesSymbol, names);
    
    UNPROTECT(1);

    /** sortlist -- not relevant **/

    for (i=0;i<2*(nvar+1);i++)
        InByteBinary(fp,1);
    
    /** format list
	passed back to R as attributes.
	Useful to identify date variables.
    **/

    PROTECT(tmp=allocVector(STRSXP,nvar));
    for (i=0;i<nvar;i++){
        InStringBinary(fp,12,timestamp);
	/* STRING(tmp)[i]=mkChar(timestamp);*/
	SET_STRING_ELT(tmp,i,mkChar(timestamp));
    }
    setAttrib(df,install("formats"),tmp);
    UNPROTECT(1);

    /** value labels.  These are stored as the names of label formats, 
	which are themselves stored later in the file.  Not implemented**/
 
    for(i=0;i<nvar;i++){
        InStringBinary(fp,9,aname);
    }
	

    /** Variable Labels **/
    
    PROTECT(varlabels=allocVector(STRSXP,nvar));

    if (version5){
        for(i=0;i<nvar;i++) {
            InStringBinary(fp,32,datalabel);
	    /* STRING(varlabels)[i]=mkChar(datalabel);*/
	    SET_STRING_ELT(varlabels,i,mkChar(datalabel));
	}
    } else {
        for(i=0;i<nvar;i++) {
            InStringBinary(fp,81,datalabel);
	    /* STRING(varlabels)[i]=mkChar(datalabel);*/
	    SET_STRING_ELT(varlabels,i,mkChar(datalabel));
	}
    }
    setAttrib(df, install("var.labels"), varlabels);
    
    UNPROTECT(1);

    /** variable 'characteristics'  -- not yet implemented **/

    while(InByteBinary(fp,1)) {
        charlen= (InShortIntBinary(fp,1,swapends));
	for (i=0;i<charlen;i++)
	  InByteBinary(fp,1);
    }
    charlen=(InShortIntBinary(fp,1,swapends));
    if (charlen!=0)
      error("Something strange in the file\n (Type 0 characteristic of nonzero length)");


    /** The Data **/


    for(i=0;i<nobs;i++){
        for(j=0;j<nvar;j++){
	    switch (INTEGER(types)[j]) {
	    case STATA_FLOAT:
		/* REAL(VECTOR(df)[j])[i]=(InFloatBinary(fp,0,swapends));*/
		REAL(VECTOR_ELT(df,j))[i]=(InFloatBinary(fp,0,swapends));
		break;
	    case STATA_DOUBLE:
	        REAL(VECTOR_ELT(df,j))[i]=(InDoubleBinary(fp,0,swapends));
		break;
	    case STATA_INT:
	        INTEGER(VECTOR_ELT(df,j))[i]=(InIntegerBinary(fp,0,swapends));
		break;
	    case STATA_SHORTINT:
	        INTEGER(VECTOR_ELT(df,j))[i]=(InShortIntBinary(fp,0,swapends));
		break;
	    case STATA_BYTE:
	        INTEGER(VECTOR_ELT(df,j))[i]=(int) InByteBinary(fp,0);
		break;
	    default:
	        charlen=INTEGER(types)[j]-STATA_STRINGOFFSET;
	        PROTECT(tmp=allocString(charlen+1));
		InStringBinary(fp,charlen,CHAR(tmp));
		CHAR(tmp)[charlen]=0;
		SET_STRING_ELT(VECTOR_ELT(df,j),i,tmp);
		UNPROTECT(1);
	      break;
	    }
	}
    }  
    PROTECT(tmp = mkString("data.frame"));
    setAttrib(df, R_ClassSymbol, tmp);
    UNPROTECT(1);
    PROTECT(row_names = allocVector(STRSXP, nobs));
    for (i=0; i<nobs; i++) {
        sprintf(datalabel, "%d", i+1);
        /*STRING(row_names)[i] = mkChar(datalabel);*/
        SET_STRING_ELT(row_names,i,mkChar(datalabel));
    }
    setAttrib(df, R_RowNamesSymbol, row_names);
    UNPROTECT(1);     

    UNPROTECT(2); /* types, df */

    return(df);

}
SEXP do_readStata(SEXP call)
{ 
    SEXP fname,  result;
    FILE *fp;

    if ((sizeof(double)!=8) | (sizeof(int)!=4) | (sizeof(float)!=4))
      error("can't yet read Stata .dta on this platform");


    if (!isValidString(fname = CADR(call)))
	error("first argument must be a file name\n");

    fp = fopen(R_ExpandFileName(CHAR(STRING_ELT(fname,0))), "rb");
    if (!fp)
	error("unable to open file");
    result = R_LoadStataData(fp);
    fclose(fp);
    return result;
}


/** low level output **/

static void OutIntegerBinary(int i, FILE * fp, int naok)
{
    i=((i==NA_INTEGER) & !naok ? STATA_INT_NA : i);
    if (fwrite(&i, sizeof(int), 1, fp) != 1)
	error("a binary write error occured");

}

static void OutByteBinary(unsigned char i, FILE * fp)
{ 
    if (fwrite(&i, sizeof(char), 1, fp) != 1)
	error("a binary write error occured");
}

static void OutShortIntBinary(int i,FILE * fp)
{
  unsigned char first,second;
  
  if (endian==LOHI){
    first= (i>>8);
    second=i & 0xff;
  } 
  else {
    first=i & 0xff;
    second=i>>8;
  }
  if (fwrite(&first, sizeof(char), 1, fp) != 1)
    error("a binary write error occured");
  if (fwrite(&second, sizeof(char), 1, fp) != 1)
    error("a binary write error occured");
}


static void  OutDoubleBinary(double d, FILE * fp, int naok)
{
    d=(R_FINITE(d) ? d : STATA_DOUBLE_NA);
    if (fwrite(&d, sizeof(double), 1, fp) != 1)
	error("a binary write error occured");
}


static void OutStringBinary(char *buffer, FILE * fp, int nchar)
{
    if (fwrite(buffer, nchar, 1, fp) != 1)
	error("a binary write error occured");
}

static char* nameMangleOut(char *stataname, int len){
    int i;
    for(i=0;i<len;i++){
      if (stataname[i]=='.') stataname[i]='_';
    }
    return stataname;
}

void R_SaveStataData(FILE *fp, SEXP df)
{
    int i,j,k,l,nvar,nobs,charlen;
    char datalabel[81]="Written by R.              ", timestamp[18], aname[9];
    char format9g[12]="%9.0g", strformat[12]="";
    SEXP names,types;
    
    k=0; /* -Wall */


    setup_consts();  /*endianness*/

    /** first write the header **/
    
    OutByteBinary((char) 108,fp);            /* release */
    OutByteBinary((char) endian,fp);
    OutByteBinary(1,fp);            /* filetype */
    OutByteBinary(0,fp);            /* padding */

    nvar=length(df);
    OutShortIntBinary(nvar,fp);
    nobs=length(VECTOR_ELT(df,0));
    OutIntegerBinary(nobs,fp,1);  /* number of cases */
    OutStringBinary(datalabel,fp,81);   /* data label - zero terminated string */
    for(i=0;i<18;i++){
      timestamp[i]=0;
    }
    OutStringBinary(timestamp,fp,18);   /* file creation time - zero terminated string */
  
   
    
    /** write variable descriptors **/
    
    /** types **/
    /* FIXME: writes everything as double or integer to save effort*/
    
    PROTECT(types=allocVector(INTSXP,nvar));

    for(i=0;i<nvar;i++){
      switch(TYPEOF(VECTOR_ELT(df,i))){
        case LGLSXP:
        case INTSXP:
	  OutByteBinary(STATA_INT,fp);
	  break;
	case REALSXP:
	  OutByteBinary(STATA_DOUBLE,fp);
	  break;
        case STRSXP:
	  charlen=0;
	  for(j=0;j<nobs;j++){
	    k=length(STRING_ELT(VECTOR_ELT(df,i),j));
	    if (k>charlen)
	      charlen=k;
	  }
	  OutByteBinary((unsigned char)(k+STATA_STRINGOFFSET),fp);
	  INTEGER(types)[i]=k;
	  break;
	default:
	  error("Unknown data type");
	  break;
      }
    }

    /** names truncated to 8 characters**/
    
    PROTECT(names=getAttrib(df,R_NamesSymbol));
    for (i=0;i<nvar;i++){
 	strncpy(aname,CHAR(STRING_ELT(names,i)),8);
        OutStringBinary(nameMangleOut(aname,8),fp,8);
	OutByteBinary(0,fp);
    }



    /** sortlist -- not relevant **/

    for (i=0;i<2*(nvar+1);i++)
        OutByteBinary(0,fp);
    
    /** format list: arbitrarily write numbers as %9g format
	but strings need accurate types */
    for (i=0;i<nvar;i++){
        if (TYPEOF(VECTOR_ELT(df,i))==STRSXP){
          /* string types are at most 128 characters
              so we can't get a buffer overflow in sprintf **/	   
	    sprintf(strformat,"%%%ds",INTEGER(types)[i]);
	    OutStringBinary(strformat,fp,12);
	} else { 
	    OutStringBinary(format9g,fp,12);
	}
    }

    /** value labels.  These are stored as the names of label formats, 
	which are themselves stored later in the file.  Not implemented**/
 
    for(i=0;i<9;i++)
      aname[i]=(char) 0;
    for(i=0;i<nvar;i++){
        OutStringBinary(aname,fp,9);
    }
	

    /** Variable Labels -- full R name of column**/
     

    for(i=0;i<nvar;i++) {
        strncpy(datalabel,CHAR(STRING_ELT(names,i)),81);
	datalabel[80]=(char) 0;
        OutStringBinary(datalabel,fp,81);
    }
    UNPROTECT(1); /*names*/


    

    /** variable 'characteristics' -- not relevant**/
    OutByteBinary(0,fp);
    OutByteBinary(0,fp);
    OutByteBinary(0,fp);


    /** The Data **/


    for(i=0;i<nobs;i++){
        for(j=0;j<nvar;j++){
	    switch (TYPEOF(VECTOR_ELT(df,j))) {
	    case LGLSXP:
	        OutIntegerBinary(LOGICAL(VECTOR_ELT(df,j))[i],fp,0);
		break;
	    case INTSXP:
	        OutIntegerBinary(INTEGER(VECTOR_ELT(df,j))[i],fp,0);
		break;
	    case REALSXP:
	        OutDoubleBinary(REAL(VECTOR_ELT(df,j))[i],fp,0);
		break;
	    case STRSXP:
	        k=length(STRING_ELT(VECTOR_ELT(df,j),i));
	        OutStringBinary(CHAR(STRING_ELT(VECTOR_ELT(df,j),i)),fp,k);
		for(l=INTEGER(types)[j]-k;l>0;l--)
		    OutByteBinary(0,fp);
	        break;
	    default:
	        error("This can't happen.");
	        break;
	    }
	}
    }  
    UNPROTECT(1); /*types*/


}

SEXP do_writeStata(SEXP call)
{ 
    SEXP fname,  df;
    FILE *fp;

    if ((sizeof(double)!=8) | (sizeof(int)!=4) | (sizeof(float)!=4))
      error("can't yet read write .dta on this platform");


    if (!isValidString(fname = CADR(call)))
	error("first argument must be a file name\n");


    fp = fopen(R_ExpandFileName(CHAR(STRING_ELT(fname,0))), "wb");
    if (!fp)
	error("unable to open file");
 
    df=CADDR(call);
    if (!inherits(df,"data.frame"))
        error("data to be saved must be in a data frame.");
 
    R_SaveStataData(fp,df);
    fclose(fp);
    return R_NilValue;
}
