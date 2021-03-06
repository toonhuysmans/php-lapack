/*
  +----------------------------------------------------------------------+
  | PHP Version 5 / lapac                                                |
  +----------------------------------------------------------------------+
  | Copyright (c) 2012 Ian Barber                                        |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Ian Barber <ian.barber@gmail.com>                           |
  +----------------------------------------------------------------------+
*/

#include "php_lapack.h"
#include "php_lapack_internal.h"
#include "php_ini.h" /* needed for 5.2 */
#include "Zend/zend_exceptions.h"
#include "ext/standard/info.h"

#include "cblas.h"

static zend_class_entry *php_lapack_sc_entry;
static zend_class_entry *php_lapack_exception_sc_entry;
static zend_object_handlers lapack_object_handlers;

#define LAPACK_THROW(message, code) \
		zend_throw_exception(php_lapack_exception_sc_entry, message, (long)code TSRMLS_CC); \
		return;


/* --- Helper Functions --- */

/* {{{ static long* php_lapack_linearize_array(zval *inarray, int *m, int *n)
Transform a PHP array into linear array of longs, and return dimensions 
*/
static double* php_lapack_linearize_array(zval *inarray, int *m, int *n) 
{
	double *outarray; 
	zval **ppzval;
	zval **ppinnerval;
	int i, j;
	
	/* Set rows num */
	*m = zend_hash_num_elements(Z_ARRVAL_P(inarray));
	*n = 0;
	outarray = NULL;
	j = 0;
	i = 0;

	if (*m > 0) {
		for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(inarray));
			 zend_hash_get_current_data(Z_ARRVAL_P(inarray), (void **) &ppzval) == SUCCESS;
			 zend_hash_move_forward(Z_ARRVAL_P(inarray))) {
		
			if (Z_TYPE_PP(ppzval) == IS_ARRAY) {
			
				if (outarray == NULL) {
					/* Set columns num and alloc memory for value */
					*n = zend_hash_num_elements(Z_ARRVAL_PP(ppzval));
					if(*n == 0) {
						return outarray;
					}
					outarray = safe_emalloc(*m * *n, sizeof(double), 0);
				} else if (zend_hash_num_elements(Z_ARRVAL_PP(ppzval)) != *n) {
					/* The matrix is not valid */
					efree(outarray);
					return NULL;
				}
			
				j = 0;
				for (zend_hash_internal_pointer_reset(Z_ARRVAL_PP(ppzval));
					 zend_hash_get_current_data(Z_ARRVAL_PP(ppzval), (void **) &ppinnerval) == SUCCESS;
					 zend_hash_move_forward(Z_ARRVAL_PP(ppzval))) {
						convert_to_double(*ppinnerval);
						outarray[(j * *m) + i] = Z_DVAL_PP(ppinnerval);
						j++;
				}
			
			}
			
			i++;
		}
	}
	
	return outarray;
}
/* }}} */

/* {{{ static zval* php_lapack_reassemble_array(zval *return_value, double *inarray, int m, int n)
Loop through a long array and reassemble into a square php 2d array based on
the height and width supplied
*/
static void php_lapack_reassemble_array(zval *return_value, double *inarray, int m, int n, int stride) 
{
	zval *inner;
	int height, width;
	height = width = 0;
	
	array_init(return_value);
	
	for( height = 0; height < m; height++ ) {
		MAKE_STD_ZVAL(inner);
		array_init(inner);
		for( width = 0; width < n; width++ ) {
			add_next_index_double(inner, inarray[height+(width*stride)]);
		}
		add_next_index_zval(return_value, inner);
	}
	
	return;
}
/* }}} */

/* {{{ static double* php_lapack_identity( long m )
Generate an identity linear identity matrix
*/
static double* php_lapack_identity( long m ) 
{
	int i, j;
	double *outarray;
	
	outarray = safe_emalloc(m * m, sizeof(double), 0);
	
	for ( i = 0; i < m; i++ ) {
		for ( j = 0; j < m; j++ ) {
			outarray[(j*m)+i] = j == i ? 1.0 : 0.0;
		}
	}
	
	return outarray;
}
/* }}} */


/* --- Lapack Matrix Utility Functions --- */

/* {{{ array Lapack::pseudoInverse(array A);
Find the pseudoinverse of a matrix A. 
*/
PHP_METHOD(Lapack, pseudoInverse)
{
	zval *a;
	double *al;
	lapack_int info,m,n,lda,ldb,nrhs;
	lapack_int *ipiv;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &a) == FAILURE) {
		return;
	}
	
	al = php_lapack_linearize_array(a, &m, &n);
	if (al == NULL) {
		LAPACK_THROW("Invalid input matrix - argument 1", 102);
	}
	
	ipiv = safe_emalloc(m, sizeof(lapack_int), 0);
	lda = m;
	ldb = m;
	nrhs = m;
	
	info = LAPACKE_dgetrf( LAPACK_COL_MAJOR, m, n, al, lda, ipiv);
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	}
	
	info = LAPACKE_dgetri( LAPACK_COL_MAJOR, n, al, lda, ipiv);	
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} else if (info == 0) {
		/* If success, fill the data. If not, there is an error so we return empty array */
		php_lapack_reassemble_array(return_value, al, m, n, lda);
	}
	
	efree(al);
	efree(ipiv);
	
	return;
}
/* }}} */

/* {{{ array Lapack::identity(int i);
Return an identity matrix size i.
*/
PHP_METHOD(Lapack, identity)
{
	double *al;
	long m;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &m) == FAILURE) {
		return;
	}
	
	if ( m < 1 ) {
		LAPACK_THROW("Invalid input size - must be 1 or greater", 102);
	}
	
	al = php_lapack_identity(m);
	
	php_lapack_reassemble_array(return_value, al, m, m, m);
	
	efree(al);
	
	return;
}
/* }}} */

/* --- Lapack Linear Equation Functions --- */

/* {{{ array Lapack::solveLinearEquation(array A, array B);
This function computes the solution to the system of linear
equations with a square matrix A and multiple
right-hand sides B 
*/
PHP_METHOD(Lapack, solveLinearEquation)
{
	zval *a, *b;
	double *al, *bl;
	lapack_int info,m,n,lda,ldb,nrhs;
	lapack_int *ipiv;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "aa", &a, &b) == FAILURE) {
		return;
	}
	
	al = php_lapack_linearize_array(a, &m, &n);
	if (al == NULL) {
		LAPACK_THROW("Invalid input matrix - argument 1", 102);
	}
	
	bl = php_lapack_linearize_array(b, &m, &nrhs);
	if (bl == NULL) {
		efree(al);
		LAPACK_THROW("Invalid input matrix - argument 2", 102);
	}
	
	ipiv = safe_emalloc(m, sizeof(lapack_int), 0);
	lda = n;
	ldb = n;
	
	info = LAPACKE_dgesv( LAPACK_COL_MAJOR, n, nrhs, al, lda, ipiv, bl, ldb);
		
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} else if (info == 0) {
		/* If success, fill the data. If not, there is an error so we return empty array */
		php_lapack_reassemble_array(return_value, bl, n, nrhs, ldb);
	}
	
	efree(al);
	efree(bl);
	efree(ipiv);
	
	return;
}
/* }}} */

/* --- Lapack Linear Least Squares Functions --- */

/* {{{ array Lapack::leastSquaresByFactorisation(array A, array B);
Solve the linear least squares problem, find min x in || B - Ax || 
Returns an array representing x. Expects arrays of arrays, and will 
return an array of arrays in the dimension B num cols x A num cols. 
Uses QR or LQ factorisation on matrix A. 
*/
PHP_METHOD(Lapack, leastSquaresByFactorisation)
{
	zval *a, *b;
	double *al, *bl;
	lapack_int info,m,n,lda,ldb,nrhs;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "aa", &a, &b) == FAILURE) {
		return;
	}
	
	al = php_lapack_linearize_array(a, &m, &n);
	if (al == NULL) {
		LAPACK_THROW("Invalid input matrix - argument 1", 102);
	}
	
	bl = php_lapack_linearize_array(b, &m, &nrhs);
	if (bl == NULL) {
		efree(al);
		LAPACK_THROW("Invalid input matrix - argument 2", 102);
	}
	
	/* For rowmajor it would be: */
	/* lda = n; ldb = nrhs; */
	lda = m;
	ldb = m;
	
	info = LAPACKE_dgels( LAPACK_COL_MAJOR, 'N', m, n, nrhs, al, lda, bl, ldb);
		
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} else if (info == 0) {
		/* If success, fill the data. If not, there is an error so we return empty array */
		php_lapack_reassemble_array(return_value, bl, n, nrhs, ldb);
	}
	
	efree(al);
	efree(bl);
	
	return;
}
/* }}} */

/* {{{ array Lapack::leastSquaresBySVD(array A, array B);
Solve the linear least squares problem, find min x in || B - Ax || 
Returns an array representing x. Expects arrays of arrays, and will 
return an array of arrays in the dimension B num cols x A num cols. 
Uses SVD with a divide and conquer algorithm. 
*/
PHP_METHOD(Lapack, leastSquaresBySVD)
{
	zval *a, *b;
	double *al, *bl, *s;
	lapack_int info,m,n,lda,ldb,nrhs,rank;
	/* Negative rcond means using default (machine precision) value */
	double rcond = -1.0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "aa", &a, &b) == FAILURE) {
		return;
	}
	
	al = php_lapack_linearize_array(a, &m, &n);
	if (al == NULL) {
		LAPACK_THROW("Invalid input matrix - argument 1", 102);
	}
	
	bl = php_lapack_linearize_array(b, &m, &nrhs);
	if (bl == NULL) {
		efree(al);
		LAPACK_THROW("Invalid input matrix - argument 2", 102);
	}
	
	/* For rowmajor it would be: */
	/* lda = n; ldb = nrhs; */
	/* Maybe m n*/
	lda = m;
	ldb = m;
	s = safe_emalloc(m, sizeof(double), 0);
	
	info = LAPACKE_dgelsd ( LAPACK_COL_MAJOR, m, n, nrhs, al, lda, bl, ldb, s, rcond, &rank );
		
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} else if (info == 0) {
		/* 
			can assemble the singular values if we want:  
			php_lapack_reassemble_array(return_value, s, 1, (n < m ? n : m), ldb);
			for now we are just getting the LLS solution
		*/
		php_lapack_reassemble_array(return_value, bl, n, nrhs, ldb);
	}
	
	efree(al);
	efree(bl);
	efree(s);
	
	return;
}
/* }}} */

/* --- Lapack Eigenvalues and SVD Functions --- */

/* {{{ array Lapack::eigenValues(array A, [array &leftEigenvectors, array &rightEigenvectors]);
Calculate the eigenvalues for the given matrix. Can optionaly return the eigenvectors for the 
matrix. 
*/
PHP_METHOD(Lapack, eigenValues) 
{
	zval *a, *inner, *row, *col, *leig, *reig;
	double *al, *wr, *wi, *vl, *vr;
	lapack_int info, m, n, lda, ldvl, ldvr;
	int idx, j;
	
	leig = reig = NULL;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a|a!a!", &a, &leig, &reig) == FAILURE) {
		return;
	}
	
	al = php_lapack_linearize_array(a, &m, &n);
	if (al == NULL) {
		LAPACK_THROW("Invalid input matrix", 102);
	} else if ( m != n ) { 
		LAPACK_THROW("Matrix must be square", 103);
	}
	
	lda = n;
	ldvl = n;
	ldvr = n;
	
	wr = safe_emalloc((n < m ? n : m), sizeof(double), 0);
	wi = safe_emalloc(ldvl * n, sizeof(double), 0);
	vr = safe_emalloc(ldvl * n, sizeof(double), 0);
	vl = safe_emalloc(ldvr * n, sizeof(double), 0);
	
	info = LAPACKE_dgeev( LAPACK_COL_MAJOR, 'V', 'V', n, al, lda, wr, wi, vl, ldvl, vr, ldvr );
	
	array_init(return_value);	
	
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} else if (info == 0) {
		
		/* Returning the eigenvalues alone */
		for( idx = 0; idx < n; idx++ ) {
			MAKE_STD_ZVAL(inner);
			array_init(inner);
			add_next_index_double(inner, wr[idx]);
			if( wi[idx] != (float)0.0 ) {
				add_next_index_double(inner, wi[idx]);
			}
			add_next_index_zval(return_value, inner);
		}
		
		/* Return left eigenvectors */
		if (leig != NULL && Z_TYPE_P(leig) == IS_ARRAY) { 
			for( idx = 0; idx < n; idx++ ) {
				MAKE_STD_ZVAL(row);
				array_init(row);
				j = 0;
				while( j < n ) {
					if( wi[j] != (float)0.0 ) {
						MAKE_STD_ZVAL(col);
						array_init(col);
						add_next_index_double(col, vl[idx+j*ldvl]);
						add_next_index_double(col, vl[idx+(j+1)*ldvl]);
						add_next_index_zval(row, col);
						MAKE_STD_ZVAL(col);
						array_init(col);
						add_next_index_double(col, vl[idx+j*ldvl]);
						add_next_index_double(col, -vl[idx+(j+1)*ldvl]);
						add_next_index_zval(row, col);
						j += 2;
					} else {
						MAKE_STD_ZVAL(col);
						array_init(col);
						add_next_index_double(col, vl[idx+j*ldvl]);
						add_next_index_zval(row, col);
						j++;
					}
				}
				add_next_index_zval(leig, row);
			}
		}
		
		/* Return right eigenvector */
		if (reig != NULL && Z_TYPE_P(reig) == IS_ARRAY) {
			for( idx = 0; idx < n; idx++ ) {
				MAKE_STD_ZVAL(row);
				array_init(row);
				j = 0;
				while( j < n ) {
					if( wi[j] != (float)0.0 ) {
						MAKE_STD_ZVAL(col);
						array_init(col);
						add_next_index_double(col, vr[idx+j*ldvr]);
						add_next_index_double(col, vr[idx+(j+1)*ldvr]);
						add_next_index_zval(row, col);
						MAKE_STD_ZVAL(col);
						array_init(col);
						add_next_index_double(col, vr[idx+j*ldvr]);
						add_next_index_double(col, -vr[idx+(j+1)*ldvr]);
						add_next_index_zval(row, col);
						j += 2;
					} else {
						MAKE_STD_ZVAL(col);
						array_init(col);
						add_next_index_double(col, vr[idx+j*ldvr]);
						add_next_index_zval(row, col);
						j++;
					}
				}
				add_next_index_zval(reig, row);
			}
		}
	}
	
	efree(al);
	efree(wr);
	efree(wi);
	efree(vl);
	efree(vr);	
	
	return;
}
/* }}} */

/* {{{ array Lapack::singularValues(array A);
Calculate the singular values of the matrix A. 
*/
PHP_METHOD(Lapack, singularValues) 
{
	zval *a;
	double *al, *s, *u, *vt;
	lapack_int info, m, n, lda, ldu, ldvt;
	
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &a) == FAILURE) {
		return;
	}
	
	al = php_lapack_linearize_array(a, &m, &n);
	if (al == NULL) {
		LAPACK_THROW("Invalid input matrix", 102);
	}
	
	lda = m;
	ldu = m;
	ldvt = n;
	s = safe_emalloc((n < m ? n : m), sizeof(double), 0);
	u = safe_emalloc(ldu * m, sizeof(double), 0);
	vt = safe_emalloc(ldvt*n, sizeof(double), 0);
	
	info = LAPACKE_dgesdd( LAPACK_COL_MAJOR, 'S', m, n, al, lda, s, u, ldu, vt, ldvt );
	
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) {
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} else if (info == 0) {
		php_lapack_reassemble_array(return_value, s, 1, n, 1);
	}
	
	efree(al);
	efree(s);
	efree(u);
	efree(vt);
	
	return;	
}
/* }}} */

/* {{{ array Lapack::shapeRegressionModel(array M, array P, array W);
Calculate a regression model between the measurements M and the 3D shapes
represented by the Principal Components (PCs) in P and the PC weights in W.
Returns an array representing the regression equations/model in matrix form. 
Expects arrays of arrays, with M number of subjects by number of measures, 
P 3 times number of vertices by number of PCs, W number of subjects by number 
of PCs, and will return an array of arrays in the dimension three times number 
of vertices by number of measures plus 1. Uses SVD of M internally. 
*/
PHP_METHOD(Lapack, shapeRegressionModel)
{
	zval *M, *P, *W;
	double *Ml, *Pl, *Wl, *Fl, *S, *U, *VT, *superb, *Sinv, *T1, *T2, *T3, *R;
	int i, j, k;

	// ns = number of subjects, nf = number of features/measurements, 
	// np = number of principal components, nc = number of coordinate values
	lapack_int info,n,m,ns,nf,np,nc,ldM,ldP,ldW,ldF,ldU,ldVT;

	/* Negative rcond means using default (machine precision) value */
	double rcond = -1.0;

	// parse paremeters
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "aaa", &M, &P, &W) == FAILURE) {
		return;
	}
	
	Ml = php_lapack_linearize_array(M, &ns, &nf);
	if (Ml == NULL) {
		LAPACK_THROW("Invalid input matrix - argument 1 (M)", 102);
	}
	
	Pl = php_lapack_linearize_array(P, &nc, &np);
	if (Pl == NULL) {
		efree(Ml);
		LAPACK_THROW("Invalid input matrix - argument 2 (P)", 102);
	}
	
	Wl = php_lapack_linearize_array(W, &m, &n);
	if (Wl == NULL) {
		efree(Ml);
		efree(Pl);
		LAPACK_THROW("Invalid input matrix - argument 3 (W)", 102);
	}
	if( m != ns )
	{
		LAPACK_THROW("Invalid input matrix - argument 3 (W), wrong number of rows", 102);	
	}
	if( n != np )
	{
		LAPACK_THROW("Invalid input matrix - argument 3 (W), wrong number of columns", 102);	
	}

	// create matrix F which is M transposed with additional row of ones.
	Fl = safe_emalloc((nf + 1) * ns, sizeof(double), 0);

	for ( i = 0; i < nf+1; i++ ) 
	{
		for ( j = 0; j < ns; j++ ) 
		{
			k = (j * (nf+1)) + i;
			Fl[k] = (i < nf) ? Ml[(i * ns) + j] : 1.0;
		}
	}

	// do svd of F
	ldF = nf+1;
	
	S = safe_emalloc(nf+1, sizeof(double), 0);

	ldU = nf+1;
	U = safe_emalloc((nf + 1) * (nf + 1), sizeof(double), 0);

	ldVT = nf + 1;
	VT = safe_emalloc((nf + 1) * ns , sizeof(double), 0);

	superb = safe_emalloc(nf , sizeof(double), 0);

	info = LAPACKE_dgesvd ( LAPACK_COL_MAJOR, 'S', 'S', nf + 1, ns, Fl, ldF, S, U, ldU,
              				VT, ldVT, superb );
	if ( info == LAPACK_WORK_MEMORY_ERROR || info == LAPACK_TRANSPOSE_MEMORY_ERROR ) 
	{
		LAPACK_THROW("Not enough memory to calculate result", 101);
	} 
	else if (info > 0) 
	{
		LAPACK_THROW("SVD failed", 101);
	}

	// Step by step mulitiplication of R = P . W^T . V . S^-1 . U^T

	// T1 = S^-1 . U^T
	T1 = safe_emalloc( (nf+1) * (nf+1), sizeof(double), 0);

	Sinv = safe_emalloc( (nf+1) * (nf+1), sizeof(double), 0);
	for ( i = 0; i < nf+1; i++ ) 
	{
		Sinv[ i * (nf + 1) + i ] = 1.0 / S[i];
	}

    cblas_dgemm( CblasColMajor,  CblasNoTrans, CblasTrans, (nf + 1), (nf + 1), (nf + 1),
                   1.0, Sinv, (nf + 1), U, (nf + 1), 0.0, T1, (nf + 1) );

	// T2 = V . T1 = VT^T . T1
    T2 = safe_emalloc( ns * (nf+1), sizeof(double), 0);

    cblas_dgemm( CblasColMajor,  CblasTrans, CblasNoTrans, ns, (nf + 1), (nf + 1),
                   1.0, VT, (nf + 1), T1, (nf + 1), 0.0, T2, ns );

	// T3 = W^T . T2
    T3 = safe_emalloc( np * (nf+1), sizeof(double), 0);

    cblas_dgemm( CblasColMajor,  CblasTrans, CblasNoTrans, np, (nf + 1), ns,
                   1.0, Wl, ns, T2, ns, 0.0, T3, np );

	// R = P . T3
 	R = safe_emalloc( nc * (nf+1), sizeof(double), 0);

    cblas_dgemm( CblasColMajor,  CblasNoTrans, CblasNoTrans, nc, (nf + 1), np,
                   1.0, Pl, nc, T3, np, 0.0, R, nc );

	// assemble matrices for output
	php_lapack_reassemble_array(return_value, R, nc, nf+1, nc);
	
	efree(Ml);
	efree(Pl);
	efree(Wl);
	efree(S);
	efree(U);
	efree(VT);
	efree(superb);
	efree(Sinv);
	efree(T1);
	efree(T2);
	efree(T3);
	efree(R);

	return;
}
/* }}} */

/* --- ARGUMENTS AND INIT --- */

ZEND_BEGIN_ARG_INFO_EX(lapack_empty_args, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(lapack_lls_args, 0, 0, 2)
	ZEND_ARG_INFO(0, a)
	ZEND_ARG_INFO(0, b)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(lapack_values_args, 0, 0, 2)
	ZEND_ARG_INFO(0, a)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(lapack_eigen_args, 0, 0, 1)
	ZEND_ARG_INFO(0, a)
	ZEND_ARG_INFO(0, left)
	ZEND_ARG_INFO(0, right)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(lapack_srm_args, 0, 0, 3)
	ZEND_ARG_INFO(0, M)
	ZEND_ARG_INFO(0, P)
	ZEND_ARG_INFO(0, W)
ZEND_END_ARG_INFO()

static zend_function_entry php_lapack_class_methods[] =
{
	PHP_ME(Lapack, solveLinearEquation,			lapack_lls_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, leastSquaresByFactorisation,	lapack_lls_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, leastSquaresBySVD,			lapack_lls_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, eigenValues,					lapack_eigen_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, singularValues,				lapack_values_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, identity,					lapack_values_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, pseudoInverse,				lapack_values_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_ME(Lapack, shapeRegressionModel,		lapack_srm_args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	{ NULL, NULL, NULL }
};

PHP_MINIT_FUNCTION(lapack)
{
	zend_class_entry ce;
	memcpy(&lapack_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));

	INIT_CLASS_ENTRY(ce, "Lapack", php_lapack_class_methods);
	ce.create_object = NULL;
	lapack_object_handlers.clone_obj = NULL;
	php_lapack_sc_entry = zend_register_internal_class(&ce TSRMLS_CC);
	
	INIT_CLASS_ENTRY(ce, "Lapackexception", NULL);
	php_lapack_exception_sc_entry = zend_register_internal_class_ex(&ce, zend_exception_get_default(TSRMLS_C), NULL TSRMLS_CC);
	php_lapack_exception_sc_entry->ce_flags |= ZEND_ACC_FINAL;

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(lapack)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(lapack)
{
	php_info_print_table_start();
		php_info_print_table_header(2, "LAPACK extension", "enabled");
		php_info_print_table_row(2, "LAPACK extension version", PHP_LAPACK_EXTVER);
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

/* No global functions */
zend_function_entry lapack_functions[] = {
	{NULL, NULL, NULL} 
};

zend_module_entry lapack_module_entry =
{
	STANDARD_MODULE_HEADER,
	PHP_LAPACK_EXTNAME,
	lapack_functions,				/* Functions */
	PHP_MINIT(lapack),				/* MINIT */
	PHP_MSHUTDOWN(lapack),			/* MSHUTDOWN */
	NULL,						/* RINIT */
	NULL,						/* RSHUTDOWN */
	PHP_MINFO(lapack),				/* MINFO */
	PHP_LAPACK_EXTVER,				/* version */
	STANDARD_MODULE_PROPERTIES
};


#ifdef COMPILE_DL_LAPACK
ZEND_GET_MODULE(lapack)
#endif /* COMPILE_DL_LAPACK */
