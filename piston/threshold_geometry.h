/*
Copyright (c) 2011, Los Alamos National Security, LLC
All rights reserved.
Copyright 2011. Los Alamos National Security, LLC. This software was produced under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National Laboratory (LANL),
which is operated by Los Alamos National Security, LLC for the U.S. Department of Energy. The U.S. Government has rights to use, reproduce, and distribute this software.

NEITHER THE GOVERNMENT NOR LOS ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.

If software is modified to produce derivative works, such modified software should be clearly marked, so as not to confuse it with the version available from LANL.

Additionally, redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
·         Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
·         Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other
          materials provided with the distribution.
·         Neither the name of Los Alamos National Security, LLC, Los Alamos National Laboratory, LANL, the U.S. Government, nor the names of its contributors may be used
          to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/iterator/permutation_iterator.h>
#include <thrust/transform_scan.h>
#include <thrust/binary_search.h>
#include <thrust/tuple.h>
#include <thrust/sort.h>
#include <thrust/unique.h>

#include <piston/image3d.h>
#include <piston/piston_math.h>
#include <piston/choose_container.h>
#include <piston/hsv_color_map.h>

// FixME: the input type may be 3-tuple of float or int,
struct tuple2float4 : thrust::unary_function<thrust::tuple<float, float, float>, float4>
{
    template <typename Tuple>
    __host__ __device__
    float4 operator()(Tuple xyz) const {
	return make_float4((float) thrust::get<0>(xyz),
	                   (float) thrust::get<1>(xyz),
	                   (float) thrust::get<2>(xyz),
	                   1.0f);
    }
};

namespace piston
{

/* Terminologies:
 * 	Boundary: cells at the borders of the input dataset
 * 	Valid   : cells with all the vertices passing the threshold
 * 	Invalid : cells with at least one vertices failing the threshold
 * 	Exterior valid : valid cells that generate geometry
 * 	Interior valid : valid cells that don't generate geometry, they are valid
 * 	cells surrounded by other valid cells, i.e. the number of valid neighbors is 6 */
template <typename InputDataSet>
struct threshold_geometry
{
    typedef typename InputDataSet::PointDataIterator InputPointDataIterator;
    typedef typename InputDataSet::PhysicalCoordinatesIterator InputGridCoordinatesIterator;

    typedef typename thrust::iterator_difference<InputPointDataIterator>::type	diff_type;
    typedef typename thrust::iterator_space<InputPointDataIterator>::type	space_type;
    typedef typename thrust::iterator_value<InputPointDataIterator>::type	value_type;

    typedef typename thrust::counting_iterator<int, space_type>	CountingIterator;

    typedef typename detail::choose_container<InputPointDataIterator, int>::type  IndicesContainer;
    typedef typename detail::choose_container<InputPointDataIterator, int>::type  ValidFlagsContainer;

    typedef typename IndicesContainer::iterator IndicesIterator;
    typedef typename ValidFlagsContainer::iterator ValidFlagsIterator;
    typedef thrust::permutation_iterator<typename thrust::transform_iterator<tuple2float4, InputGridCoordinatesIterator>, IndicesIterator> VerticesIterator;

    typedef typename detail::choose_container<InputPointDataIterator, float3>::type	NormalsContainer;
    typedef typename NormalsContainer::iterator  NormalsIterator;
    typedef thrust::permutation_iterator<InputPointDataIterator, IndicesIterator> ScalarsIterator;

    InputDataSet &input;
    float min_value;
    float max_value;
    bool colorFlip;

    ValidFlagsContainer valid_cell_flags;
    IndicesContainer    valid_cell_enum;
    IndicesContainer    valid_cell_indices;
    IndicesContainer    num_valid_cell_neighbors;
    ValidFlagsContainer exterior_cell_flags;
    IndicesContainer    exterior_cell_enum;
    IndicesContainer    exterior_cell_indices;
    IndicesContainer	vertices_indices;
    NormalsContainer 	normals;

    bool useInterop;
    float4 *vertexBufferData;
    float3 *normalBufferData;
    float4 *colorBufferData;
    int vboSize;
#ifdef USE_INTEROP
    GLuint vboBuffers[3];
    struct cudaGraphicsResource* vboResources[3];
#endif


    float minThresholdRange, maxThresholdRange;
    unsigned int num_total_vertices;

    threshold_geometry(InputDataSet &input, float min_value, float max_value ) :
	input(input), min_value(min_value), max_value(max_value), colorFlip(false), useInterop(false), vboSize(0)
    {
    }

    void freeMemory(bool includeInput=true)
    {
      valid_cell_flags.clear();  valid_cell_enum.clear();  valid_cell_indices.clear();
      num_valid_cell_neighbors.clear();
      exterior_cell_flags.clear();  exterior_cell_enum.clear();  exterior_cell_indices.clear();
      vertices_indices.clear(); normals.clear();
    }

    void operator()() {
	const int NCells = input.NCells;

	valid_cell_flags.resize(NCells);

	// test and enumerate cells that pass threshold, we don't do kernel fusion
	// because the flags are used in a later stage.
	thrust::transform(CountingIterator(0), CountingIterator(0)+NCells,
	                  valid_cell_flags.begin(),
	                  threshold_cell(input, min_value, max_value));

	// enumerate valid cells
	valid_cell_enum.resize(NCells);
	thrust::inclusive_scan(valid_cell_flags.begin(), valid_cell_flags.end(),
	                       valid_cell_enum.begin());
	// the total number of valid cells is the last element of the enumeration.
	int num_valid_cells = valid_cell_enum.back();

	// no valid cells at all, return with empty vertices vector.
	if (num_valid_cells == 0) {
	    vertices_indices.clear();
	    normals.clear();
	    return;
	}

	// generate indices to cells that pass threshold
	// This ends the core part of the threshold filter, the rest is
	// representation and mapper to geometry.
	// TODO: should we move it to something like GoemetryFilter as VTK?
	valid_cell_indices.resize(num_valid_cells);
	thrust::upper_bound(valid_cell_enum.begin(), valid_cell_enum.end(),
	                    CountingIterator(0), CountingIterator(0)+num_valid_cells,
	                    valid_cell_indices.begin());

	// calculate how many neighbors of a cell are valid.
	num_valid_cell_neighbors.resize(num_valid_cells);
	thrust::transform(valid_cell_indices.begin(), valid_cell_indices.end(),
	                  num_valid_cell_neighbors.begin(),
	                  valid_cell_neighbors(input, thrust::raw_pointer_cast(&*valid_cell_flags.begin())));

	// test if a cell is at the exterior of the blob of valid cells.
	exterior_cell_flags.resize(num_valid_cells);
	thrust::transform(num_valid_cell_neighbors.begin(), num_valid_cell_neighbors.end(),
	                  exterior_cell_flags.begin(),
	                  is_exterior_cell());

	// enumerate how many exterior cells we have
	exterior_cell_enum.resize(num_valid_cells);
	thrust::inclusive_scan(exterior_cell_flags.begin(), exterior_cell_flags.end(),
	                       exterior_cell_enum.begin());

	// total number of exterior cells
	int num_exterior_cells = exterior_cell_enum.back();
	//std::cout << "number of exterior cells: " << num_exterior_cells << std::endl;

	// search for indices to exterior cells among all valid cells
	exterior_cell_indices.resize(num_exterior_cells);
	thrust::upper_bound(exterior_cell_enum.begin(), exterior_cell_enum.end(),
	                    CountingIterator(0), CountingIterator(0)+num_exterior_cells,
	                    exterior_cell_indices.begin());

	num_total_vertices = num_exterior_cells*24;
	//std::cout << "number of vertices: " << numTotalVertices << std::endl;

	if (useInterop) {
#if USE_INTEROP
	    if (num_total_vertices > vboSize)
	    {
              glBindBuffer(GL_ARRAY_BUFFER, vboBuffers[0]);
              glBufferData(GL_ARRAY_BUFFER, num_total_vertices*sizeof(float4), 0, GL_DYNAMIC_DRAW);
              if (glGetError() == GL_OUT_OF_MEMORY) { std::cout << "Out of VBO memory" << std::endl; exit(-1); }
	      glBindBuffer(GL_ARRAY_BUFFER, vboBuffers[1]);
	      glBufferData(GL_ARRAY_BUFFER, num_total_vertices*sizeof(float4), 0, GL_DYNAMIC_DRAW);
	      if (glGetError() == GL_OUT_OF_MEMORY) { std::cout << "Out of VBO memory" << std::endl; exit(-1); }
	      glBindBuffer(GL_ARRAY_BUFFER, vboBuffers[2]);
	      glBufferData(GL_ARRAY_BUFFER, num_total_vertices*sizeof(float3), 0, GL_DYNAMIC_DRAW);
	      if (glGetError() == GL_OUT_OF_MEMORY) { std::cout << "Out of VBO memory" << std::endl; exit(-1); }
	      glBindBuffer(GL_ARRAY_BUFFER, 0);
	      vboSize = num_total_vertices;
	    }
	    size_t num_bytes;
	    cudaGraphicsMapResources(1, &vboResources[0], 0);
	    cudaGraphicsResourceGetMappedPointer((void **) &vertexBufferData,
						 &num_bytes, vboResources[0]);

	    if (vboResources[1]) {
		cudaGraphicsMapResources(1, &vboResources[1], 0);
		cudaGraphicsResourceGetMappedPointer((void **) &colorBufferData,
						     &num_bytes,
						     vboResources[1]);
	    }

	    cudaGraphicsMapResources(1, &vboResources[2], 0);
	    cudaGraphicsResourceGetMappedPointer((void **) &normalBufferData,
						 &num_bytes, vboResources[2]);
#endif
	}

	// generate 6 quards for each exterior cell
	vertices_indices.resize(num_total_vertices);
	normals.resize(num_total_vertices);
	thrust::for_each(thrust::make_zip_iterator(thrust::make_tuple(CountingIterator(0),
	                                                              thrust::make_permutation_iterator(valid_cell_indices.begin(), exterior_cell_indices.begin()))),
	                 thrust::make_zip_iterator(thrust::make_tuple(CountingIterator(0) + num_exterior_cells,
	                                                              thrust::make_permutation_iterator(valid_cell_indices.begin(), exterior_cell_indices.begin()))),
	                 generate_quads(input, thrust::raw_pointer_cast(&*vertices_indices.begin()), thrust::raw_pointer_cast(&*normals.begin())));

	if (useInterop) {
#if USE_INTEROP
	    thrust::copy(vertices_begin(),
	                 vertices_end(),
	                 thrust::device_ptr<float4>(vertexBufferData));
	    thrust::copy(normals_begin(), normals_end(),
			 thrust::device_ptr<float3>(normalBufferData));
	    thrust::transform(scalars_begin(), scalars_end(),
		    thrust::device_ptr<float4>(colorBufferData),
		    color_map<float>(minThresholdRange, maxThresholdRange, colorFlip));

	    for (int i = 0; i < 3; i++)
		cudaGraphicsUnmapResources(1, &vboResources[i], 0);
#endif
	}
    }

    // FixME: the input data type should really be cells rather than cell_ids
    // FixME: change float to value_type
    struct threshold_cell : public thrust::unary_function<int, bool>
    {
	// FixME: constant iterator and/or iterator to const problem.
	InputPointDataIterator point_data;
	const float min_value;
	const float max_value;

	const int xdim;
	const int ydim;
	const int zdim;
	const int cells_per_layer;

	__host__ __device__
	bool threshold(float val) const {
	    return (min_value <= val) && (val <= max_value);
	}

	threshold_cell(InputDataSet &input, float min_value, float max_value) :
	    point_data(input.point_data_begin()),
	    min_value(min_value), max_value(max_value),
	    xdim(input.dim0), ydim(input.dim1), zdim(input.dim2),
	    cells_per_layer((xdim - 1) * (ydim - 1)) {}

	__host__ __device__
	bool operator() (int cell_id) const {
	    const int x = cell_id % (xdim - 1);
	    const int y = (cell_id / (xdim - 1)) % (ydim -1);
	    const int z = cell_id / cells_per_layer;

	    // indices to the eight vertices of the voxel
	    const int i0 = x    + y*xdim + z * xdim * ydim;
	    const int i1 = i0   + 1;
	    const int i2 = i0   + 1	+ xdim;
	    const int i3 = i0   + xdim;

	    const int i4 = i0   + xdim * ydim;
	    const int i5 = i1   + xdim * ydim;
	    const int i6 = i2   + xdim * ydim;
	    const int i7 = i3   + xdim * ydim;

	    // scalar values of the eight vertices
	    const float f0 = *(point_data + i0);
	    const float f1 = *(point_data + i1);
	    const float f2 = *(point_data + i2);
	    const float f3 = *(point_data + i3);
	    const float f4 = *(point_data + i4);
	    const float f5 = *(point_data + i5);
	    const float f6 = *(point_data + i6);
	    const float f7 = *(point_data + i7);

	    // a cell is considered passing the threshold if all of its vertices
	    // are passing the threshold.
	    bool valid = threshold(f0);
	    valid &= threshold(f1);
	    valid &= threshold(f2);
	    valid &= threshold(f3);
	    valid &= threshold(f4);
	    valid &= threshold(f5);
	    valid &= threshold(f6);
	    valid &= threshold(f7);

	    return valid;
	}
    };

    // return the number of neighbors that are valid cells
    struct valid_cell_neighbors : public thrust::unary_function<int, int>
    {
	const int xdim;
	const int ydim;
	const int zdim;
	const int cells_per_layer;

	const int *valid_cell_flags;

	valid_cell_neighbors(InputDataSet &input, const int *valid_cell_flags) :
	    xdim(input.dim0), ydim(input.dim1), zdim(input.dim2),
	    cells_per_layer((xdim - 1) * (ydim - 1)),
	    valid_cell_flags(valid_cell_flags) {}

	__host__ __device__
	int operator() (int valid_cell_id) const {
	    // cell ids of the cell's six neighbors
	    const int n0 = valid_cell_id - (xdim - 1);
	    const int n1 = valid_cell_id + 1;
	    const int n2 = valid_cell_id + (xdim - 1);
	    const int n3 = valid_cell_id - 1;
	    const int n4 = valid_cell_id - cells_per_layer;
	    const int n5 = valid_cell_id + cells_per_layer;

	    // we are using fixed boundary conditions here, if a cell is at
	    // the border of the data set, it DOES NOT have a valid cell
	    // neighbor at that face.
	    const int x = valid_cell_id % (xdim - 1);
	    const int y = (valid_cell_id / (xdim - 1)) % (ydim -1);
	    const int z = valid_cell_id / cells_per_layer;

	    // we are taking advantage of short-circuit && operator here,
	    // the coordinates of the cell is tested first so we won't
	    // access to data past boundary.
	    int num_valid_cells = !(y == 0)          && *(valid_cell_flags + n0);
	    num_valid_cells    += !(x == (xdim - 2)) && *(valid_cell_flags + n1);
	    num_valid_cells    += !(y == (ydim - 2)) && *(valid_cell_flags + n2);
	    num_valid_cells    += !(x == 0)          && *(valid_cell_flags + n3);
	    num_valid_cells    += !(z == 0)          && *(valid_cell_flags + n4);
	    num_valid_cells    += !(z == (zdim - 2)) && *(valid_cell_flags + n5);

	    return num_valid_cells;
	}
    };

    // return true if the cell will actually generate geometry.
    struct is_exterior_cell : public thrust::unary_function<int, bool>
    {
	__host__ __device__
	bool operator() (int num_valid_cell_neighbors) const {
	    return num_valid_cell_neighbors != 6;
	}
    };

    // FixME: the input data type should really be cells rather than cell_ids
    struct generate_quads : public thrust::unary_function<thrust::tuple<int, int>, void>
    {
    private:
	InputGridCoordinatesIterator physical_coord;

	const int xdim;
	const int ydim;
	const int zdim;
	const int cells_per_layer;

	// crazy C++ const correctness, vertices_indices is a pointer that
	// does not change the address it points to.
	int * const vertices_indices;
	float3 * const normals_output;

	template <typename Tuple>
	static __host__ __device__
	float3 tuple2float3(const Tuple& xyz) {
	    return make_float3((float) thrust::get<0>(xyz),
	                       (float) thrust::get<1>(xyz),
	                       (float) thrust::get<2>(xyz));
	}

    public:
	generate_quads(InputDataSet &input, int * const vertices_indices, float3 * const normals) :
	    physical_coord(input.physical_coordinates_begin()),
	    xdim(input.dim0), ydim(input.dim1), zdim(input.dim2),
	    cells_per_layer((xdim - 1) * (ydim - 1)),
	    vertices_indices(vertices_indices), normals_output(normals) {}

	__host__ __device__
	void operator() (const thrust::tuple<int, int>& indices_tuple) const {
	    const int exterior_cell_id = thrust::get<0>(indices_tuple);
	    const int global_cell_id   = thrust::get<1>(indices_tuple);

	    const int vertices_for_faces[] =
	    {
		 0, 1, 5, 4, // face 0
		 1, 2, 6, 5, // face 1
		 2, 3, 7, 6, // face 2
		 0, 4, 7, 3, // face 3
		 0, 3, 2, 1, // face 4
		 4, 5, 6, 7  // face 5
	    };

	    const int i = global_cell_id % (xdim - 1);
	    const int j = (global_cell_id / (xdim - 1)) % (ydim -1);
	    const int k = global_cell_id / cells_per_layer;

	    // indices to the eight vertices of the voxel
	    int indices[8];
	    indices[0] = i            + j*xdim  + k * xdim * ydim;
	    indices[1] = indices[0]   + 1;
	    indices[2] = indices[0]   + 1	+ xdim;
	    indices[3] = indices[0]   + xdim;

	    indices[4] = indices[0]   + xdim * ydim;
	    indices[5] = indices[1]   + xdim * ydim;
	    indices[6] = indices[2]   + xdim * ydim;
	    indices[7] = indices[3]   + xdim * ydim;

	    for (int v = 0; v < 24; v++) {
		*(vertices_indices + exterior_cell_id*24 + v) = indices[vertices_for_faces[v]];
	    }

	    // calculate surface normal by cross product of the diagonals of the quad.
	    float3 p[8];
	    p[0] = tuple2float3(*(physical_coord + indices[0]));
	    p[1] = tuple2float3(*(physical_coord + indices[1]));
	    p[2] = tuple2float3(*(physical_coord + indices[2]));
	    p[3] = tuple2float3(*(physical_coord + indices[3]));
	    p[4] = tuple2float3(*(physical_coord + indices[4]));
	    p[5] = tuple2float3(*(physical_coord + indices[5]));
	    p[6] = tuple2float3(*(physical_coord + indices[6]));
	    p[7] = tuple2float3(*(physical_coord + indices[7]));

	    for (int v = 0; v < 24; v += 4) {
		const float3 edge0 = p[vertices_for_faces[v+2]] - p[vertices_for_faces[v]];
		const float3 edge1 = p[vertices_for_faces[v+3]] - p[vertices_for_faces[v+1]];
		const float3 normal = normalize(cross(edge0, edge1));
		*(normals_output + exterior_cell_id*24 + v) =
		*(normals_output + exterior_cell_id*24 + v + 1) =
		*(normals_output + exterior_cell_id*24 + v + 2) =
		*(normals_output + exterior_cell_id*24 + v + 3) = normal;
	    }
	}
    };

    // FixME: better name!!!
    VerticesIterator vertices_begin() {
	return thrust::make_permutation_iterator(thrust::make_transform_iterator(input.physical_coordinates_begin(),
	                                                                         tuple2float4()),
	                                         vertices_indices.begin());
    }
    VerticesIterator vertices_end() {
	return thrust::make_permutation_iterator(thrust::make_transform_iterator(input.physical_coordinates_begin(),
	                                                                         tuple2float4()),
	                                         vertices_indices.begin()) + vertices_indices.size();
    }

    // FixME: better name!!!
    NormalsIterator normals_begin() {
	return normals.begin();
    }
    NormalsIterator normals_end() {
	return normals.end();
    }

    // FixME: better name!!!
    ScalarsIterator scalars_begin() {
	return thrust::make_permutation_iterator(input.point_data_begin(), vertices_indices.begin());
    }
    ScalarsIterator scalars_end() {
	return thrust::make_permutation_iterator(input.point_data_begin(), vertices_indices.begin()) +
		vertices_indices.size();
    }

    // Set threshold rangle
    void set_threshold_range(float minVal, float maxVal) {
        min_value = minVal; max_value = maxVal;
    }
};

}
