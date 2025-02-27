#pragma once

#include "utils.h"

#define INDEX_TYPE uint32_t
#define KEY_TYPE uint64_t

namespace puff {


    template<typename IndexType, typename ValueType, typename MemorySpace>
    class SparseMatrixWrapper {
        public:
            SparseMatrixWrapper() {}

            SparseMatrixWrapper(SparseMatrix<IndexType, ValueType, MemorySpace> matrix) {
                matrix = matrix;
            }
            
            size_t get_num_rows() {
                return matrix.num_rows;
            }

            size_t get_num_cols() {
                return matrix.num_cols;
            }

            size_t get_num_entries() {
                return matrix.num_entries;
            }


            void insert_entry(IndexType row, IndexType col, ValueType val) {
                auto row_col_string = row_col_to_key(row, col);
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    entries[row_col_string] = val;
                }
            }

            void remove_entry(IndexType row, IndexType col) {
                auto row_col_string = row_col_to_key(row, col);
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    entries.erase(row_col_string);
                }
            }

            void make_matrix()
            {
                std::lock_guard<std::mutex> lock(mtx);
                Vector<IndexType, cusp::host_memory> h_I(entries.size());
                Vector<IndexType, cusp::host_memory> h_J(entries.size());
                Vector<ValueType, cusp::host_memory> h_V(entries.size());

                int nnz = 0;
                for(auto& [row_col_key, value] : entries)
                {
                    auto [row, col] = key_to_row_col(row_col_key);
                    if(value == ValueType(0)) continue; // No 0 element
                    h_I[nnz] = row;
                    h_J[nnz] = col;
                    h_V[nnz] = value;
                    nnz++;
                }    

                {
                    // Free memory
                    std::unordered_map<KEY_TYPE, ValueType> temp_entries;
                    std::swap(entries, temp_entries); // force entries to free memory
                }  

                h_I.resize(nnz); h_I.shrink_to_fit();
                h_J.resize(nnz); h_J.shrink_to_fit();
                h_V.resize(nnz); h_V.shrink_to_fit();
                
                
                // sort triplets by (i,j) index using two stable sorts (first by J, then by I)
                thrust::stable_sort_by_key(h_J.begin(), h_J.end(), thrust::make_zip_iterator(thrust::make_tuple(h_I.begin(), h_V.begin())));
                thrust::stable_sort_by_key(h_I.begin(), h_I.end(), thrust::make_zip_iterator(thrust::make_tuple(h_J.begin(), h_V.begin())));


                // Calculate num_Rows
                IndexType numRows = *thrust::max_element(h_I.begin(), h_I.end()) + 1; // Assume row indices are 0-based
                IndexType numCols = *thrust::max_element(h_J.begin(), h_J.end()) + 1; // Assume column indices are 0-based
                /*****************CSR Matrix********************/
                /*
                Vector<IndexType, cusp::host_memory> h_row_ptrs(numRows + 1, 0); // Initialize row pointers with zeros
                IndexType currentIndex = 0;
                IndexType currentRow = std::numeric_limits<IndexType>::max();
                for (IndexType i = 0; i < h_I.size(); ++i) {
                    if (h_I[i] != currentRow) { // New row encountered
                        currentRow = h_I[i];
                        h_row_ptrs[currentRow] = currentIndex; // Update row pointer for the new row
                    }
                    currentIndex++;
                }
                h_row_ptrs[numRows] = h_I.size(); // Set the last element of row_ptrs

                
                // resize matrix
                matrix.resize(numRows, numCols, h_V.size());
                // Move to prescribed memory space
                // Insert I to matrix.row_offsets
                thrust::copy(h_row_ptrs.begin(), h_row_ptrs.end(), matrix.row_offsets.begin());
                // Insert J to matrix.column_indices
                thrust::copy(h_J.begin(), h_J.end(), matrix.column_indices.begin());
                // Insert V to matrix.values
                thrust::copy(h_V.begin(), h_V.end(), matrix.values.begin());
                */

                /*****************COO Matrix********************/
                // resize matrix
                matrix.resize(numRows, numCols, h_V.size());
                // Move to prescribed memory space
                // Insert I to matrix.row_offsets
                thrust::copy(h_I.begin(), h_I.end(), matrix.row_indices.begin());
                // Insert J to matrix.column_indices
                thrust::copy(h_J.begin(), h_J.end(), matrix.column_indices.begin());
                // Insert V to matrix.values
                thrust::copy(h_V.begin(), h_V.end(), matrix.values.begin());

                // Make the transpose coo_matrix view
                permutation = Vector<IndexType, MemorySpace>(cusp::counting_array<IndexType>(matrix.num_entries));
                Vector<IndexType, MemorySpace> matrix_column_indices(matrix.column_indices);
                cusp::counting_sort_by_key(matrix_column_indices, permutation, IndexType(0), IndexType(matrix.num_cols));
                matrix_t = cusp::make_coo_matrix_view(matrix.num_rows, matrix.num_cols, matrix.num_entries,
                                                cusp::make_array1d_view(thrust::make_permutation_iterator(matrix.column_indices.begin(), permutation.begin()),
                                                                    thrust::make_permutation_iterator(matrix.column_indices.begin(), permutation.end())),
                                                cusp::make_array1d_view(thrust::make_permutation_iterator(matrix.row_indices.begin(),    permutation.begin()),
                                                                    thrust::make_permutation_iterator(matrix.row_indices.begin(),    permutation.end())),
                                                cusp::make_array1d_view(thrust::make_permutation_iterator(matrix.values.begin(),         permutation.begin()),
                                                                      thrust::make_permutation_iterator(matrix.values.begin(),         permutation.end())));
            }

            void reset()
            {
                std::lock_guard<std::mutex> lock(mtx);
                SparseMatrix<IndexType, ValueType, MemorySpace> temp_matrix(0, 0, 0);
                swap(matrix, temp_matrix); // force matrix to free memory
                
                std::unordered_map<KEY_TYPE, ValueType> temp_entries;
                swap(entries, temp_entries); // force entries to free memory
            }

            void print_matrix() {
                std::lock_guard<std::mutex> lock(mtx);
                cusp::print(matrix);
            }

            void SpMV(Vector<ValueType, MemorySpace>& x, 
                      Vector<ValueType, MemorySpace>& y, 
                      bool transpose = false, 
                      bool conjugate = false) {     
                
                if constexpr(std::is_same_v<ValueType, dcomplex> ||
                             std::is_same_v<ValueType, fcomplex> ||
                             std::is_same_v<ValueType, hcomplex> ||
                             std::is_same_v<ValueType, bcomplex>)
                {
                    if(conjugate)
                        thrust::transform(matrix.values.begin(), matrix.values.end(), matrix.values.begin(), conjugate_functor<ValueType>());
                }

                
                if(&x == &y) 
                {
                    Vector<ValueType, MemorySpace> temp(x.size());
                    if (transpose) 
                        cusp::multiply(matrix_t, x, temp);
                    else
                        cusp::multiply(matrix, x, temp);
                    y.swap(temp);
                }
                else
                {
                    if (transpose)
                        cusp::multiply(matrix_t, x, y);
                    else
                        cusp::multiply(matrix, x, y);
                }
                
                if constexpr(std::is_same_v<ValueType, dcomplex> ||
                             std::is_same_v<ValueType, fcomplex> ||
                             std::is_same_v<ValueType, hcomplex> ||
                             std::is_same_v<ValueType, bcomplex>)
                {
                    if(conjugate)
                        thrust::transform(matrix.values.begin(), matrix.values.end(), matrix.values.begin(), conjugate_functor<ValueType>());
                }
            }


            void SpMVP(ValueType alpha, 
                       Vector<ValueType, MemorySpace>& x, 
                       ValueType beta, 
                       Vector<ValueType, MemorySpace>& y, 
                       bool transpose = false, 
                       bool conjugate = false) {
                // y = alpha * A * x + beta * y
                if (beta == 0)
                {
                    // y = A * x
                    if(alpha != 0) 
                        SpMV(x, y, transpose, conjugate); 
                    // y *= alpha;
                    if(alpha != 1)
                        thrust::transform(y.begin(), y.end(), thrust::make_constant_iterator(alpha), y.begin(), thrust::multiplies<ValueType>());
                }
                else
                {
                    Vector<ValueType, MemorySpace> temp(x.size(), 0);
                    // temp = A * x
                    if(alpha != 0) 
                        SpMV(x, temp, transpose, conjugate); 
                    // temp = alpha * temp
                    if(alpha != 0 && alpha != 1)
                        thrust::transform(temp.begin(), temp.end(), thrust::make_constant_iterator(alpha), temp.begin(), thrust::multiplies<ValueType>());
                    // y = beta * y
                    if(beta != 1)
                        thrust::transform(y.begin(), y.end(), thrust::make_constant_iterator(beta), y.begin(), thrust::multiplies<ValueType>());
                    // y = temp + y
                    thrust::transform(temp.begin(), temp.end(), y.begin(), y.begin(), thrust::plus<ValueType>());
                }
            }

            // Return the spectral radius (maximum of the absolute eigenvalues) of the matrix
            ValueType spectral_radius(size_t k = 10, bool symmetric = false) 
            {
                return cusp::eigen::ritz_spectral_radius(matrix, k, symmetric);
            }

            // Solving Ax = b using GMRES
            typedef typename cusp::norm_type<ValueType>::type Real; // Real is the type of the residual norm
            ValueType gmres(Vector<ValueType, MemorySpace>& x, 
                            Vector<ValueType, MemorySpace>& b, 
                            size_t restart = 50, 
                            size_t maxiter = 1000, 
                            Real tol = Real(1e-6), 
                            bool verbose = false) 
            {
                cusp::monitor<Real> monitor(b, maxiter, tol, 0, verbose);
                cusp::krylov::gmres(matrix, x, b, restart, monitor);
                return monitor.residual_norm();
            }


        private:
            SparseMatrix<IndexType, ValueType, MemorySpace> matrix;
            // Transpose matrix view
            // Use decltype to infer the type
            SparseMatrixView<IndexType, ValueType, MemorySpace> matrix_t;
            Vector<IndexType, MemorySpace> permutation; // permutation for transpose

            std::unordered_map<KEY_TYPE, ValueType> entries;
            // mutex lock
            std::mutex mtx;

            KEY_TYPE row_col_to_key(IndexType row, IndexType col) {
                // shift row by 32 bits and add col
                return ((KEY_TYPE)row << 32) + col;
            }

            std::pair<IndexType, IndexType> key_to_row_col(const KEY_TYPE& key)
            {
                IndexType row = static_cast<IndexType>(key >> 32);
                IndexType col = static_cast<IndexType>(key);
                return std::make_pair(row, col);
            }
            
    };






    template<typename ValueType>
    using SparseMatrix_h = SparseMatrixWrapper<INDEX_TYPE, ValueType, cusp::host_memory>;

    template<typename ValueType>
    using SparseMatrix_d = SparseMatrixWrapper<INDEX_TYPE, ValueType, cusp::device_memory>;

    template<typename ValueType>
    using Vector_h = Vector<ValueType, cusp::host_memory>;

    template<typename ValueType>
    using Vector_d = Vector<ValueType, cusp::device_memory>;
    
}
