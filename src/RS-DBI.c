/*
 * Copyright (C) 1999-2002 The Omega Project for Statistical Computing
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "RS-DBI.h"

/* TODO: monitor memory/object size consumption against S limits
 *       in $SHOME/include/options.h we find "max_memory". We then
 *       size to make sure we're not bumping into problems.
 *       But, is size() reliable?  How should we do this?
 *
 * TODO: invoke user-specified generators
 *
 * TODO: Implement exception objects for each dbObject.
 */

static RS_DBI_manager *dbManager = NULL;

Mgr_Handle *
RS_DBI_allocManager(const char *drvName, int max_con,
  int fetch_default_rec, int force_realloc)
{
  /* Currently, the dbManager is a singleton (therefore we don't
   * completly free all the space).  Here we alloc space
   * for the dbManager and return its mgrHandle.  force_realloc
   * means to re-allocate number of connections, etc. (in this case
   * we require to have all connections closed).  (Note that if we
   * re-allocate, we don't re-set the counter, and thus we make sure
   * we don't recycle connection Ids in a giver S/R session).
   */
  Mgr_Handle     *mgrHandle;
  RS_DBI_manager *mgr;
  int counter;
  int mgr_id = (int) getpid();
  int i;

  mgrHandle = RS_DBI_asMgrHandle(mgr_id);

  if(!dbManager){                      /* alloc for the first time */
    counter = 0;                       /* connections handled so far */
    mgr = (RS_DBI_manager*) malloc(sizeof(RS_DBI_manager));
  }
  else {                               /* we're re-entering */
    if(dbManager->connections){        /* and mgr is valid */
      if(!force_realloc)
	return mgrHandle;
      else
	RS_DBI_freeManager(mgrHandle);  /* i.e., free connection arrays*/
    }
    counter = dbManager->counter;
    mgr = dbManager;
  }
 /* Ok, we're here to expand number of connections, etc.*/
  if(!mgr)
    RS_DBI_errorMessage("could not malloc the dbManger", RS_DBI_ERROR);
  mgr->drvName = RS_DBI_copyString(drvName);
  mgr->drvData = (void *) NULL;
  mgr->managerId = mgr_id;
  mgr->connections =  (RS_DBI_connection **)
    calloc((size_t) max_con, sizeof(RS_DBI_connection));
  if(!mgr->connections){
    free(mgr);
    RS_DBI_errorMessage("could not calloc RS_DBI_connections", RS_DBI_ERROR);
  }
  mgr->connectionIds = (int *) calloc((size_t)max_con, sizeof(int));
  if(!mgr->connectionIds){
    free(mgr->connections);
    free(mgr);
    RS_DBI_errorMessage("could not calloc vector of connection Ids",
          RS_DBI_ERROR);
  }
  mgr->counter = counter;
  mgr->length = max_con;
  mgr->num_con = (int) 0;
  mgr->fetch_default_rec = fetch_default_rec;
  for(i=0; i < max_con; i++){
    mgr->connectionIds[i] = -1;
    mgr->connections[i] = (RS_DBI_connection *) NULL;
  }

  dbManager = mgr;

  return mgrHandle;
}

/* We don't want to completely free the dbManager, but rather we
 * re-initialize all the fields except for mgr->counter to ensure we don't
 * re-cycle connection ids across R/S DBI sessions in the the same pid
 * (S/R session).
 */
void
RS_DBI_freeManager(Mgr_Handle *mgrHandle)
{
  RS_DBI_manager *mgr;

  mgr = RS_DBI_getManager(mgrHandle);
  if(mgr->num_con > 0){
    char *errMsg = "all opened connections were forcebly closed";
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  if(mgr->drvData){
    char *errMsg = "mgr->drvData was not freed (some memory leaked)";
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  if(mgr->drvName){
    free(mgr->drvName);
    mgr->drvName = (char *) NULL;
  }
  if(mgr->connections) {
    free(mgr->connections);
    mgr->connections = (RS_DBI_connection **) NULL;
  }
  if(mgr->connectionIds) {
    free(mgr->connectionIds);
    mgr->connectionIds = (int *) NULL;
  }
  return;
}

Con_Handle *
RS_DBI_allocConnection(Mgr_Handle *mgrHandle, int max_res)
{
  RS_DBI_manager    *mgr;
  RS_DBI_connection *con;
  Con_Handle  *conHandle;
  int  i, indx, con_id;

  mgr = RS_DBI_getManager(mgrHandle);
  indx = RS_DBI_newEntry(mgr->connectionIds, mgr->length);
  if(indx < 0){
    char buf[128], msg[128];
    (void) strcat(msg, "cannot allocate a new connection -- maximum of ");
    (void) strcat(msg, "%d connections already opened");
    (void) sprintf(buf, msg, (int) mgr->length);
    RS_DBI_errorMessage(buf, RS_DBI_ERROR);
  }
  con = (RS_DBI_connection *) malloc(sizeof(RS_DBI_connection));
  if(!con){
    char *errMsg = "could not malloc dbConnection";
    RS_DBI_freeEntry(mgr->connectionIds, indx);
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  con->managerId = MGR_ID(mgrHandle);
  con_id = mgr->counter;
  con->connectionId = con_id;
  con->drvConnection = (void *) NULL;
  con->drvData = (void *) NULL;    /* to be used by the driver in any way*/
  con->conParams = (void *) NULL;
  con->counter = (int) 0;
  con->length = max_res;           /* length of resultSet vector */

  /* result sets for this connection */
  con->resultSets = (RS_DBI_resultSet **)
    calloc((size_t) max_res, sizeof(RS_DBI_resultSet));
  if(!con->resultSets){
    char  *errMsg = "could not calloc resultSets for the dbConnection";
    RS_DBI_freeEntry(mgr->connectionIds, indx);
    free(con);
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  con->num_res = (int) 0;
  con->resultSetIds = (int *) calloc((size_t) max_res, sizeof(int));
  if(!con->resultSetIds) {
    char *errMsg = "could not calloc vector of resultSet Ids";
    free(con->resultSets);
    free(con);
    RS_DBI_freeEntry(mgr->connectionIds, indx);
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  for(i=0; i<max_res; i++){
    con->resultSets[i] = (RS_DBI_resultSet *) NULL;
    con->resultSetIds[i] = -1;
  }

  /* Finally, update connection table in mgr */
  mgr->num_con += (int) 1;
  mgr->counter += (int) 1;
  mgr->connections[indx] = con;
  mgr->connectionIds[indx] = con_id;
  conHandle = RS_DBI_asConHandle(MGR_ID(mgrHandle), con_id);
  return conHandle;
}

/* the invoking (freeing) function must provide a function for
 * freeing the conParams, and by setting the (*free_drvConParams)(void *)
 * pointer.
 */

void
RS_DBI_freeConnection(Con_Handle *conHandle)
{
  RS_DBI_connection *con;
  RS_DBI_manager    *mgr;
  int indx;

  con = RS_DBI_getConnection(conHandle);
  mgr = RS_DBI_getManager(conHandle);

  /* Are there open resultSets? If so, free them first */
  if(con->num_res > 0) {
    char *errMsg = "opened resultSet(s) forcebly closed";
    int  i;
    Res_Handle  *rsHandle;

    for(i=0; i < con->num_res; i++){
      rsHandle = RS_DBI_asResHandle(con->managerId,
				    con->connectionId,
				    (int) con->resultSetIds[i]);
      RS_DBI_freeResultSet(rsHandle);
    }
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  if(con->drvConnection) {
    char *errMsg =
      "internal error in RS_DBI_freeConnection: driver might have left open its connection on the server";
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  if(con->conParams){
    char *errMsg =
      "internal error in RS_DBI_freeConnection: non-freed con->conParams (tiny memory leaked)";
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  if(con->drvData){
    char *errMsg =
      "internal error in RS_DBI_freeConnection: non-freed con->drvData (some memory leaked)";
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  /* delete this connection from manager's connection table */
  if(con->resultSets) free(con->resultSets);
  if(con->resultSetIds) free(con->resultSetIds);

  /* update the manager's connection table */
  indx = RS_DBI_lookup(mgr->connectionIds, mgr->length, con->connectionId);
  RS_DBI_freeEntry(mgr->connectionIds, indx);
  mgr->connections[indx] = (RS_DBI_connection *) NULL;
  mgr->num_con -= (int) 1;

  free(con);
  con = (RS_DBI_connection *) NULL;

  return;
}

Res_Handle *
RS_DBI_allocResultSet(Con_Handle *conHandle)
{
  RS_DBI_connection *con = NULL;
  RS_DBI_resultSet  *result = NULL;
  Res_Handle  *rsHandle;
  int indx, res_id;

  con = RS_DBI_getConnection(conHandle);
  indx = RS_DBI_newEntry(con->resultSetIds, con->length);
  if(indx < 0){
    char msg[128], fmt[128];
    (void) strcpy(fmt, "cannot allocate a new resultSet -- ");
    (void) strcat(fmt, "maximum of %d resultSets already reached");
    (void) sprintf(msg, fmt, con->length);
    RS_DBI_errorMessage(msg, RS_DBI_ERROR);
  }

  result = (RS_DBI_resultSet *) malloc(sizeof(RS_DBI_resultSet));
  if(!result){
    char *errMsg = "could not malloc dbResultSet";
    RS_DBI_freeEntry(con->resultSetIds, indx);
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  result->drvResultSet = (void *) NULL; /* driver's own resultSet (cursor)*/
  result->drvData = (void *) NULL;   /* this can be used by driver*/
  result->statement = (char *) NULL;
  result->managerId = MGR_ID(conHandle);
  result->connectionId = CON_ID(conHandle);
  result->resultSetId = con->counter;
  result->isSelect = (int) -1;
  result->rowsAffected = (int) -1;
  result->rowCount = (int) 0;
  result->completed = (int) -1;
  result->fields = (RS_DBI_fields *) NULL;

  /* update connection's resultSet table */
  res_id = con->counter;
  con->num_res += (int) 1;
  con->counter += (int) 1;
  con->resultSets[indx] = result;
  con->resultSetIds[indx] = res_id;

  rsHandle = RS_DBI_asResHandle(MGR_ID(conHandle),CON_ID(conHandle),res_id);
  return rsHandle;
}

void
RS_DBI_freeResultSet(Res_Handle *rsHandle)
{
  RS_DBI_resultSet  *result;
  RS_DBI_connection *con;
  int indx;

  con = RS_DBI_getConnection(rsHandle);
  result = RS_DBI_getResultSet(rsHandle);

  if(result->drvResultSet) {
    char *errMsg =
      "internal error in RS_DBI_freeResultSet: non-freed result->drvResultSet (some memory leaked)";
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  if(result->drvData){
    char *errMsg =
      "internal error in RS_DBI_freeResultSet: non-freed result->drvData (some memory leaked)";
    RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
  }
  if(result->statement)
    free(result->statement);
  if(result->fields)
    RS_DBI_freeFields(result->fields);
  free(result);
  result = (RS_DBI_resultSet *) NULL;

  /* update connection's resultSet table */
  indx = RS_DBI_lookup(con->resultSetIds, con->length, RES_ID(rsHandle));
  RS_DBI_freeEntry(con->resultSetIds, indx);
  con->resultSets[indx] = (RS_DBI_resultSet *) NULL;
  con->num_res -= (int) 1;

  return;
}

RS_DBI_fields *
RS_DBI_allocFields(int num_fields)
{
  RS_DBI_fields *flds;
  size_t n;

  flds = (RS_DBI_fields *)malloc(sizeof(RS_DBI_fields));
  if(!flds){
    char *errMsg = "could not malloc RS_DBI_fields";
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  n = (size_t) num_fields;
  flds->num_fields = num_fields;
  flds->name =     (char **) calloc(n, sizeof(char *));
  flds->type =     (int *) calloc(n, sizeof(int));
  flds->length =   (int *) calloc(n, sizeof(int));
  flds->precision= (int *) calloc(n, sizeof(int));
  flds->scale =    (int *) calloc(n, sizeof(int));
  flds->nullOk =   (int *) calloc(n, sizeof(int));
  flds->isVarLength = (int *) calloc(n, sizeof(int));
  flds->Sclass =   (SEXPTYPE *) calloc(n, sizeof(SEXPTYPE));

  return flds;
}

void
RS_DBI_freeFields(RS_DBI_fields *flds)
{
  int i;
  if(flds->name) {       /* (as per Jeff Horner's patch) */
     for(i = 0; i < flds->num_fields; i++)
        if(flds->name[i]) free(flds->name[i]);
     free(flds->name);
  }
  if(flds->type) free(flds->type);
  if(flds->length) free(flds->length);
  if(flds->precision) free(flds->precision);
  if(flds->scale) free(flds->scale);
  if(flds->nullOk) free(flds->nullOk);
  if(flds->isVarLength) free(flds->isVarLength);
  if(flds->Sclass) free(flds->Sclass);
  free(flds);
  flds = (RS_DBI_fields *) NULL;
  return;
}

/* Make a data.frame from a named list by adding row.names, and class
 * attribute.  Use "1", "2", .. as row.names.
 * NOTE: Only tested  under R (not tested at all under S4 or Splus5+).
 */
void
RS_DBI_makeDataFrame(s_object *data)
{
   S_EVALUATOR

   s_object *row_names, *df_class_name;
   int   i, n;
   char   buf[1024];

   PROTECT(data);
   PROTECT(df_class_name = NEW_CHARACTER((int) 1));
   SET_CHR_EL(df_class_name, 0, C_S_CPY("data.frame"));

   /* row.names */
   n = GET_LENGTH(LST_EL(data,0));            /* length(data[[1]]) */
   PROTECT(row_names = NEW_CHARACTER(n));
   for(i=0; i<n; i++){
      (void) sprintf(buf, "%d", i+1);
      SET_CHR_EL(row_names, i, C_S_CPY(buf));
   }

   SET_ROWNAMES(data, row_names);
   SET_CLASS_NAME(data, df_class_name);

   UNPROTECT(3);
   return;
}

void
RS_DBI_allocOutput(s_object *output, RS_DBI_fields *flds,
		   int num_rec, int  expand)
{
  s_object *names, *s_tmp;
  int   j;
  int    num_fields;
  SEXPTYPE  *fld_Sclass;

  PROTECT(output);

  num_fields = flds->num_fields;
  if(expand){
    for(j = 0; j < (int) num_fields; j++){
      /* Note that in R-1.2.3 (at least) we need to protect SET_LENGTH */
      s_tmp = LST_EL(output,j);
      PROTECT(SET_LENGTH(s_tmp, num_rec));
      SET_ELEMENT(output, j, s_tmp);
      UNPROTECT(1);
    }
    UNPROTECT(1);
    return;
  }

  fld_Sclass = flds->Sclass;
  for(j = 0; j < (int) num_fields; j++){
    switch((int)fld_Sclass[j]){
    case LGLSXP:
      SET_ELEMENT(output, j, NEW_LOGICAL(num_rec));
      break;
    case STRSXP:
      SET_ELEMENT(output, j, NEW_CHARACTER(num_rec));
      break;
    case INTSXP:
      SET_ELEMENT(output, j, NEW_INTEGER(num_rec));
      break;
    case REALSXP:
      SET_ELEMENT(output, j, NEW_NUMERIC(num_rec));
      break;
    case VECSXP:
      SET_ELEMENT(output, j, NEW_LIST(num_rec));
      break;
    default:
      RS_DBI_errorMessage("unsupported data type", RS_DBI_ERROR);
    }
  }

  PROTECT(names = NEW_CHARACTER((int) num_fields));
  for(j = 0; j< (int) num_fields; j++){
    SET_CHR_EL(names,j, C_S_CPY(flds->name[j]));
  }
  SET_NAMES(output, names);

  UNPROTECT(2);

  return;
}

s_object * 		/* boolean */
RS_DBI_validHandle(Db_Handle *handle)
{
   S_EVALUATOR
   s_object  *valid;
   int  handleType = 0;

   switch( (int) GET_LENGTH(handle)){
   case MGR_HANDLE_TYPE:
     handleType = MGR_HANDLE_TYPE;
     break;
   case CON_HANDLE_TYPE:
     handleType = CON_HANDLE_TYPE;
     break;
   case RES_HANDLE_TYPE:
     handleType = RES_HANDLE_TYPE;
     break;
   }
   PROTECT(valid = NEW_LOGICAL((int) 1));
   LGL_EL(valid,0) = (int) is_validHandle(handle, handleType);
   UNPROTECT(1);
   return valid;
}

void
RS_DBI_setException(Db_Handle *handle, DBI_EXCEPTION exceptionType,
		    int errorNum, const char *errorMsg)
{
  HANDLE_TYPE handleType;

  handleType = (int) GET_LENGTH(handle);
  if(handleType == MGR_HANDLE_TYPE){
    RS_DBI_manager *obj;
    obj =  RS_DBI_getManager(handle);
    obj->exception->exceptionType = exceptionType;
    obj->exception->errorNum = errorNum;
    obj->exception->errorMsg = RS_DBI_copyString(errorMsg);
  }
  else if(handleType==CON_HANDLE_TYPE){
    RS_DBI_connection *obj;
    obj = RS_DBI_getConnection(handle);
    obj->exception->exceptionType = exceptionType;
    obj->exception->errorNum = errorNum;
    obj->exception->errorMsg = RS_DBI_copyString(errorMsg);
  }
  else {
    RS_DBI_errorMessage(
          "internal error in RS_DBI_setException: could not setException",
          RS_DBI_ERROR);
  }
  return;
}

void
RS_DBI_errorMessage(char *msg, DBI_EXCEPTION exception_type)
{
  char *driver = "RS-DBI";   /* TODO: use the actual driver name */

  switch(exception_type) {
  case RS_DBI_MESSAGE:
    warning("%s driver message: (%s)", driver, msg);
    break;
  case RS_DBI_WARNING:
    warning("%s driver warning: (%s)", driver, msg);
    break;
  case RS_DBI_ERROR:
    error("%s driver: (%s)", driver, msg);
    break;
  case RS_DBI_TERMINATE:
    error("%s driver fatal: (%s)", driver, msg); /* was TERMINATE */
    break;
  }
  return;
}

/* wrapper to strcpy */
char *
RS_DBI_copyString(const char *str)
{
  char *buffer;

  buffer = (char *) malloc((size_t) strlen(str)+1);
  if(!buffer)
    RS_DBI_errorMessage(
          "internal error in RS_DBI_copyString: could not alloc string space",
          RS_DBI_ERROR);
  return strcpy(buffer, str);
}

/* wrapper to strncpy, plus (optionally) deleting trailing spaces */
char *
RS_DBI_nCopyString(const char *str, size_t len, int del_blanks)
{
  char *str_buffer, *end;

  str_buffer = (char *) malloc(len+1);
  if(!str_buffer){
    char errMsg[128];
    (void) sprintf(errMsg,
		   "could not malloc %ld bytes in RS_DBI_nCopyString",
		   (long) len+1);
    RS_DBI_errorMessage(errMsg, RS_DBI_ERROR);
  }
  if(len==0){
    *str_buffer = '\0';
    return str_buffer;
  }

  (void) strncpy(str_buffer, str, len);

  /* null terminate string whether we delete trailing blanks or not*/
  if(del_blanks){
    for(end = str_buffer+len-1; end>=str_buffer; end--)
      if(*end != ' ') { end++; break; }
    *end = '\0';
  }
  else {
    end = str_buffer + len;
    *end = '\0';
  }
  return str_buffer;
}

s_object *
RS_DBI_copyfields(RS_DBI_fields *flds)
{
  S_EVALUATOR

  s_object *S_fields;
  int  n = (int) 8;
  char  *desc[]={"name", "Sclass", "type", "len", "precision",
 		 "scale","isVarLength", "nullOK"};
  SEXPTYPE types[] = {STRSXP, INTSXP, INTSXP,
       INTSXP, INTSXP, INTSXP,
		   LGLSXP, LGLSXP};
  int  lengths[8];
  int   i, j, num_fields;

  num_fields = flds->num_fields;
  for(j = 0; j < n; j++)
    lengths[j] = (int) num_fields;
  S_fields =  RS_DBI_createNamedList(desc, types, lengths, n);

  /* copy contentes from flds into an R/S list */
  for(i = 0; i < num_fields; i++){
    SET_LST_CHR_EL(S_fields,0,i, C_S_CPY(flds->name[i]));
    LST_INT_EL(S_fields,1,i) = (int) flds->Sclass[i];
    LST_INT_EL(S_fields,2,i) = (int) flds->type[i];
    LST_INT_EL(S_fields,3,i) = (int) flds->length[i];
    LST_INT_EL(S_fields,4,i) = (int) flds->precision[i];
    LST_INT_EL(S_fields,5,i) = (int) flds->scale[i];
    LST_INT_EL(S_fields,6,i) = (int) flds->isVarLength[i];
    LST_INT_EL(S_fields,7,i) = (int) flds->nullOk[i];
  }

  return S_fields;
}

s_object *
RS_DBI_createNamedList(char **names, SEXPTYPE *types, int *lengths, int  n)
{
  S_EVALUATOR
  s_object *output, *output_names, *obj = S_NULL_ENTRY;
  int  num_elem;
  int   j;

  PROTECT(output = NEW_LIST(n));
  PROTECT(output_names = NEW_CHARACTER(n));
  for(j = 0; j < n; j++){
    num_elem = lengths[j];
    switch((int)types[j]){
    case LGLSXP:
      PROTECT(obj = NEW_LOGICAL(num_elem));
      break;
    case INTSXP:
      PROTECT(obj = NEW_INTEGER(num_elem));
      break;
    case REALSXP:
      PROTECT(obj = NEW_NUMERIC(num_elem));
      break;
    case STRSXP:
      PROTECT(obj = NEW_CHARACTER(num_elem));
      break;
    case VECSXP:
      PROTECT(obj = NEW_LIST(num_elem));
      break;
    default:
      RS_DBI_errorMessage("unsupported data type", RS_DBI_ERROR);
    }
    SET_ELEMENT(output, (int)j, obj);
    SET_CHR_EL(output_names, j, C_S_CPY(names[j]));
  }
  SET_NAMES(output, output_names);
  UNPROTECT(n+2);
  return(output);
}

s_object *
RS_DBI_SclassNames(s_object *type)
{
  s_object *typeNames;
  int *typeCodes;
  int n;
  int  i;
  char *s;

  if(type==S_NULL_ENTRY)
     RS_DBI_errorMessage(
           "internal error in RS_DBI_SclassNames: input S types must be nonNULL",
           RS_DBI_ERROR);
  n = LENGTH(type);
  typeCodes = INTEGER_DATA(type);
  PROTECT(typeNames = NEW_CHARACTER(n));
  for(i = 0; i < n; i++) {
    s = RS_DBI_getTypeName(typeCodes[i], RS_dataTypeTable);
    if(!s){
      RS_DBI_errorMessage(
            "internal error RS_DBI_SclassNames: unrecognized S type",
            RS_DBI_ERROR);
	  s = "";
	}
    SET_CHR_EL(typeNames, i, C_S_CPY(s));
  }
  UNPROTECT(1);
  return typeNames;
}

/* The following functions roughly implement a simple object
 * database.
 */

Mgr_Handle *
RS_DBI_asMgrHandle(int mgrId)
{
  Mgr_Handle *mgrHandle;

  PROTECT(mgrHandle = NEW_INTEGER((int) 1));
  MGR_ID(mgrHandle) = mgrId;
  UNPROTECT(1);
  return mgrHandle;
}

Con_Handle *
RS_DBI_asConHandle(int mgrId, int conId)
{
  Con_Handle *conHandle;

  PROTECT(conHandle = NEW_INTEGER((int) 2));
  MGR_ID(conHandle) = mgrId;
  CON_ID(conHandle) = conId;
  UNPROTECT(1);
  return conHandle;
}

Res_Handle *
RS_DBI_asResHandle(int mgrId, int conId, int resId)
{
  Res_Handle *resHandle;

  PROTECT(resHandle = NEW_INTEGER((int) 3));
  MGR_ID(resHandle) = mgrId;
  CON_ID(resHandle) = conId;
  RES_ID(resHandle) = resId;
  UNPROTECT(1);
  return resHandle;
}

RS_DBI_manager *
RS_DBI_getManager(Mgr_Handle *handle)
{
  RS_DBI_manager  *mgr;

  if(!is_validHandle(handle, MGR_HANDLE_TYPE))
    RS_DBI_errorMessage("invalid dbManager handle", RS_DBI_ERROR);
  mgr = dbManager;
  if(!mgr)
    RS_DBI_errorMessage(
          "internal error in RS_DBI_getManager: corrupt dbManager handle",
	  RS_DBI_ERROR);
  return mgr;
}

RS_DBI_connection *
RS_DBI_getConnection(Con_Handle *conHandle)
{
  RS_DBI_manager  *mgr;
  int indx;

  mgr = RS_DBI_getManager(conHandle);
  indx = RS_DBI_lookup(mgr->connectionIds, mgr->length, CON_ID(conHandle));
  if(indx < 0)
    RS_DBI_errorMessage(
          "internal error in RS_DBI_getConnection: corrupt connection handle",
	  RS_DBI_ERROR);
  if(!mgr->connections[indx])
    RS_DBI_errorMessage(
          "internal error in RS_DBI_getConnection: corrupt connection  object",
	  RS_DBI_ERROR);
  return mgr->connections[indx];
}

RS_DBI_resultSet *
RS_DBI_getResultSet(Res_Handle *rsHandle)
{
  RS_DBI_connection *con;
  int indx;

  con = RS_DBI_getConnection(rsHandle);
  indx = RS_DBI_lookup(con->resultSetIds, con->length, RES_ID(rsHandle));
  if(indx<0)
    RS_DBI_errorMessage(
      "internal error in RS_DBI_getResultSet: could not find resultSet in connection",
      RS_DBI_ERROR);
  if(!con->resultSets[indx])
    RS_DBI_errorMessage(
          "internal error in RS_DBI_getResultSet: missing resultSet",
          RS_DBI_ERROR);
  return con->resultSets[indx];
}

/* Very simple objectId (mapping) table. newEntry() returns an index
 * to an empty cell in table, and lookup() returns the position in the
 * table of obj_id.  Notice that we decided not to touch the entries
 * themselves to give total control to the invoking functions (this
 * simplify error management in the invoking routines.)
 */
int
RS_DBI_newEntry(int *table, int length)
{
  int i, indx, empty_val;

  indx = empty_val = (int) -1;
  for(i = 0; i < length; i++)
    if(table[i] == empty_val){
      indx = i;
      break;
    }
  return indx;
}
int
RS_DBI_lookup(int *table, int length, int obj_id)
{
  int i, indx;

  indx = (int) -1;
  for(i = 0; i < length; ++i){
    if(table[i]==obj_id){
      indx = i;
      break;
    }
  }
  return indx;
}

/* return a list of entries pointed by *entries (we allocate the space,
 * but the caller should free() it).  The function returns the number
 * of entries.
 */
int
RS_DBI_listEntries(int *table, int length, int *entries)
{
  int i,n;

  for(i=n=0; i<length; i++){
    if(table[i]<0) continue;
    entries[n++] = table[i];
  }
  return n;
}
void
RS_DBI_freeEntry(int *table, int indx)
{ /* no error checking!!! */
  int empty_val = (int) -1;
  table[indx] = empty_val;
  return;
}
int
is_validHandle(Db_Handle *handle, HANDLE_TYPE handleType)
{
  int  mgr_id, len, indx;
  RS_DBI_manager    *mgr;
  RS_DBI_connection *con;

  if(IS_INTEGER(handle))
    handle = AS_INTEGER(handle);
  else
    return 0;       /* non handle object */

  len = (int) GET_LENGTH(handle);
  if(len<handleType || handleType<1 || handleType>3)
    return 0;
  mgr_id = MGR_ID(handle);
  if( ((int) getpid()) != mgr_id)
    return 0;

  /* at least we have a potential valid dbManager */
  mgr = dbManager;
  if(!mgr || !mgr->connections)  return 0;   /* expired manager*/
  if(handleType == MGR_HANDLE_TYPE) return 1;     /* valid manager id */

  /* ... on to connections */
  indx = RS_DBI_lookup(mgr->connectionIds, mgr->length, CON_ID(handle));
  if(indx<0) return 0;
  con = mgr->connections[indx];
  if(!con) return 0;
  if(!con->resultSets) return 0;       /* un-initialized (invalid) */
  if(handleType==CON_HANDLE_TYPE) return 1; /* valid connection id */

  /* .. on to resultSets */
  indx = RS_DBI_lookup(con->resultSetIds, con->length, RES_ID(handle));
  if(indx < 0) return 0;
  if(!con->resultSets[indx]) return 0;

  return 1;
}

/* The following 3 routines provide metadata for the 3 main objects
 * dbManager, dbConnection and dbResultSet.  These functions
 * an object Id and return a list with all the meta-data. In R/S we
 * simply invoke one of these and extract the metadata piece we need,
 * which can be NULL if non-existent or un-implemented.
 *
 * Actually, each driver should modify these functions to add the
 * driver-specific info, such as server version, client version, etc.
 * That's how the various RS_MySQL_managerInfo, etc., were implemented.
 */

s_object *         /* named list */
RS_DBI_managerInfo(Mgr_Handle *mgrHandle)
{
  S_EVALUATOR

  RS_DBI_manager *mgr;
  s_object *output;
  int  i, num_con;
  int n = (int) 7;
  char *mgrDesc[] = {"connectionIds", "fetch_default_rec","managerId",
		     "length", "num_con", "counter", "clientVersion"};
  SEXPTYPE mgrType[] = {INTSXP, INTSXP, INTSXP,
                        INTSXP, INTSXP, INTSXP,
                        STRSXP};
  int  mgrLen[]  = {1, 1, 1, 1, 1, 1, 1};

  mgr = RS_DBI_getManager(mgrHandle);
  num_con = (int) mgr->num_con;
  mgrLen[0] = num_con;

  output = RS_DBI_createNamedList(mgrDesc, mgrType, mgrLen, n);

  for(i = 0; i < num_con; i++)
    LST_INT_EL(output,0,i) = (int) mgr->connectionIds[i];

  LST_INT_EL(output,1,0) = (int) mgr->fetch_default_rec;
  LST_INT_EL(output,2,0) = (int) mgr->managerId;
  LST_INT_EL(output,3,0) = (int) mgr->length;
  LST_INT_EL(output,4,0) = (int) mgr->num_con;
  LST_INT_EL(output,5,0) = (int) mgr->counter;
  SET_LST_CHR_EL(output,6,0,C_S_CPY("NA"));   /* client versions? */

  return output;
}

/* The following should be considered templetes to be
 * implemented by individual drivers.
 */

s_object *        /* return a named list */
RS_DBI_connectionInfo(Con_Handle *conHandle)
{
  S_EVALUATOR

  RS_DBI_connection  *con;
  s_object *output;
  int     i;
  int  n = (int) 8;
  char *conDesc[] = {"host", "user", "dbname", "conType",
		     "serverVersion", "protocolVersion",
		     "threadId", "rsHandle"};
  SEXPTYPE conType[] = {STRSXP, STRSXP, STRSXP,
          STRSXP, STRSXP, INTSXP,
		      INTSXP, INTSXP};
  int  conLen[]  = {1, 1, 1, 1, 1, 1, 1, -1};

  con = RS_DBI_getConnection(conHandle);
  conLen[7] = con->num_res;   /* number of resultSets opened */

  output = RS_DBI_createNamedList(conDesc, conType, conLen, n);

  /* dummy */
  SET_LST_CHR_EL(output,0,0,C_S_CPY("NA"));        /* host */
  SET_LST_CHR_EL(output,1,0,C_S_CPY("NA"));        /* dbname */
  SET_LST_CHR_EL(output,2,0,C_S_CPY("NA"));        /* user */
  SET_LST_CHR_EL(output,3,0,C_S_CPY("NA"));        /* conType */
  SET_LST_CHR_EL(output,4,0,C_S_CPY("NA"));        /* serverVersion */

  LST_INT_EL(output,5,0) = (int) -1;            /* protocolVersion */
  LST_INT_EL(output,6,0) = (int) -1;            /* threadId */

  for(i=0; i < con->num_res; i++)
    LST_INT_EL(output,7,(int) i) = con->resultSetIds[i];

  return output;
}

s_object *       /* return a named list */
RS_DBI_resultSetInfo(Res_Handle *rsHandle)
{
  S_EVALUATOR

  RS_DBI_resultSet       *result;
  s_object  *output, *flds;
  int  n = (int) 6;
  char  *rsDesc[] = {"statement", "isSelect", "rowsAffected",
		     "rowCount", "completed", "fields"};
  SEXPTYPE rsType[]  = {STRSXP, INTSXP, INTSXP,
    INTSXP,   INTSXP, VECSXP};
  int  rsLen[]   = {1, 1, 1, 1, 1, 1};

  result = RS_DBI_getResultSet(rsHandle);
  if(result->fields)
    flds = RS_DBI_copyfields(result->fields);
  else
    flds = S_NULL_ENTRY;

  output = RS_DBI_createNamedList(rsDesc, rsType, rsLen, n);

  SET_LST_CHR_EL(output,0,0,C_S_CPY(result->statement));
  LST_INT_EL(output,1,0) = result->isSelect;
  LST_INT_EL(output,2,0) = result->rowsAffected;
  LST_INT_EL(output,3,0) = result->rowCount;
  LST_INT_EL(output,4,0) = result->completed;
  SET_ELEMENT(LST_EL(output, 5), (int) 0, flds);

  return output;
}

s_object *    /* named list */
RS_DBI_getFieldDescriptions(RS_DBI_fields *flds)
{
  S_EVALUATOR

  s_object *S_fields;
  int  n = (int) 7;
  int  lengths[7];
  char  *desc[]={"name", "Sclass", "type", "len", "precision",
		"scale","nullOK"};
  SEXPTYPE types[] = {STRSXP, INTSXP, INTSXP,
                      INTSXP, INTSXP, INTSXP, LGLSXP};
  int   i, j;
  int    num_fields;

  num_fields = flds->num_fields;
  for(j = 0; j < n; j++)
    lengths[j] = (int) num_fields;
  PROTECT(S_fields =  RS_DBI_createNamedList(desc, types, lengths, n));

  /* copy contentes from flds into an R/S list */
  for(i = 0; i < (int) num_fields; i++){
    SET_LST_CHR_EL(S_fields,0,i,C_S_CPY(flds->name[i]));
    LST_INT_EL(S_fields,1,i) = (int) flds->Sclass[i];
    LST_INT_EL(S_fields,2,i) = (int) flds->type[i];
    LST_INT_EL(S_fields,3,i) = (int) flds->length[i];
    LST_INT_EL(S_fields,4,i) = (int) flds->precision[i];
    LST_INT_EL(S_fields,5,i) = (int) flds->scale[i];
    LST_INT_EL(S_fields,6,i) = (int) flds->nullOk[i];
  }
  UNPROTECT(1);
  return(S_fields);
}

/* given a type id return its human-readable name.
 * We define an RS_DBI_dataTypeTable */
char *
RS_DBI_getTypeName(int t, const struct data_types table[])
{
  int i;
  char buf[128];

  for (i = 0; table[i].typeName != (char *) 0; i++) {
    if (table[i].typeId == t)
      return table[i].typeName;
  }
  sprintf(buf, "unknown type (%ld)", (long) t);
  RS_DBI_errorMessage(buf, RS_DBI_WARNING);
  return (char *) 0; /* for -Wall */
}

/* Translate R/S identifiers (and only R/S names!!!) into
 * valid SQL identifiers;  overwrite input vector. Currently,
 *   (1) translate "." into "_".
 *   (2) first character should be a letter (traslate to "X" if not),
 *       but a double quote signals a "delimited identifier"
 *   (3) check that length <= 18, but only warn, since most (all?)
 *       dbms allow much longer identifiers.
 *   (4) SQL reserved keywords are handled in the R/S calling
 *       function make.SQL.names(), not here.
 * BUG: Compound SQL identifiers are not handled properly.
 *      Note the the dot "." is a valid SQL delimiter, used for specifying
 *      user/table in a compound identifier.  Thus, it's possible that
 *      such compound name is mapped into a legal R/S identifier (preserving
 *      the "."), and then we incorrectly map such delimiting "dot" into "_"
 *      thus loosing the original SQL compound identifier.
 */
#define RS_DBI_MAX_IDENTIFIER_LENGTH 18      /* as per SQL92 */
s_object *
RS_DBI_makeSQLNames(s_object *snames)
{
   S_EVALUATOR
   long     nstrings;
   char     *name, c;
   char     errMsg[128];
   size_t   len;
   int     i;

   nstrings = (int) GET_LENGTH(snames);
   for(i = 0; i<nstrings; i++){
      name = (char *) CHR_EL(snames, i);
      if(strlen(name)> RS_DBI_MAX_IDENTIFIER_LENGTH){
	 (void) sprintf(errMsg,"SQL identifier %s longer than %d chars",
			name, RS_DBI_MAX_IDENTIFIER_LENGTH);
	 RS_DBI_errorMessage(errMsg, RS_DBI_WARNING);
      }
      /* check for delimited-identifiers (those in double-quotes);
       * if missing closing double-quote, warn and treat as non-delim
       */
      c = *name;
      len = strlen(name);
      if(c=='"' && name[len-1] =='"')
         continue;
      if(!isalpha(c) && c!='"') *name = 'X';
      name++;
      while((c=*name)){
	 /* TODO: recognize SQL delim "." instances that may have
	  * originated in SQL and R/S make.names() left alone */
	 if(c=='.') *name='_';
	 name++;
      }
   }

   return snames;
}

/*  These 2 R-specific functions are used by the C macros IS_NA(p,t)
 *  and NA_SET(p,t) (in this way one simply use macros to test and set
 *  NA's regardless whether we're using R or S.
 */
void
RS_na_set(void *ptr, SEXPTYPE type)
{
  double *d;
  int   *i;
  char   *c;
  switch(type){
  case INTSXP:
    i = (int *) ptr;
    *i = NA_INTEGER;
    break;
  case LGLSXP:
    i = (int *) ptr;
    *i = NA_LOGICAL;
    break;
  case REALSXP:
    d = (double *) ptr;
    *d = NA_REAL;
    break;
  case STRSXP:
    c = (char *) ptr;
    c = (char *) CHR_EL(NA_STRING, 0);
    break;
  }
}
int
RS_is_na(void *ptr, SEXPTYPE type)
{
   int *i, out = -2;
   char *c;
   double *d;

   switch(type){
   case INTSXP:
   case LGLSXP:
      i = (int *) ptr;
      out = (int) ((*i) == NA_INTEGER);
      break;
   case REALSXP:
      d = (double *) ptr;
      out = ISNA(*d);
      break;
   case STRSXP:
      c = (char *) ptr;
      out = (int) (strcmp(c, CHR_EL(NA_STRING, 0))==0);
      break;
   }
   return out;
}

 /* the codes come from from R/src/main/util.c */
const struct data_types RS_dataTypeTable[] = {
    { "NULL",		NILSXP	   },  /* real types */
    { "symbol",		SYMSXP	   },
    { "pairlist",	LISTSXP	   },
    { "closure",	CLOSXP	   },
    { "environment",	ENVSXP	   },
    { "promise",	PROMSXP	   },
    { "language",	LANGSXP	   },
    { "special",	SPECIALSXP },
    { "builtin",	BUILTINSXP },
    { "char",		CHARSXP	   },
    { "logical",	LGLSXP	   },
    { "integer",	INTSXP	   },
    { "double",		REALSXP	   }, /*-  "real", for R <= 0.61.x */
    { "complex",	CPLXSXP	   },
    { "character",	STRSXP	   },
    { "...",		DOTSXP	   },
    { "any",		ANYSXP	   },
    { "expression",	EXPRSXP	   },
    { "list",		VECSXP	   },
    { "raw",		RAWSXP     },
    /* aliases : */
    { "numeric",	REALSXP	   },
    { "name",		SYMSXP	   },
    { (char *)0,	-1	   }
};
