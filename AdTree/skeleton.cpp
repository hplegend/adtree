/*
*	Copyright (C) 2019 by
*       Shenglan Du (dushenglan940128@163.com)
*       Liangliang Nan (liangliang.nan@gmail.com)
*       3D Geoinformation, TU Delft, https://3d.bk.tudelft.nl
*
*	This file is part of AdTree, which implements the 3D tree
*   reconstruction method described in the following paper:
*   -------------------------------------------------------------------------------------
*       Shenglan Du, Roderik Lindenbergh, Hugo Ledoux, Jantien Stoter, and Liangliang Nan.
*       AdTree: Accurate, Detailed, and Automatic Modeling of Laser-Scanned Trees.
*       Remote Sensing. 2019, 11(18), 2074.
*   -------------------------------------------------------------------------------------
*   Please consider citing the above paper if you use the code/program (or part of it).
*
*	AdTree is free software; you can redistribute it and/or modify
*	it under the terms of the GNU General Public License Version 3
*	as published by the Free Software Foundation.
*
*	AdTree is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "skeleton.h"
#include "cylinder.h"

#include <easy3d/core/point_cloud.h>
#include <3rd_party/tetgen/tetgen.h>

#include <iostream>


#define Rand_0_1 (((double) (rand() % 1000)) / 1000)
#define max(a,b)   (((a) > (b)) ? (a) : (b))

using namespace boost;


Skeleton::Skeleton() 
    : Points_(nullptr)
    , KDtree_(nullptr)
    , Delaunay_(nullptr)
    , MST_(nullptr)
    , FineGraph_(nullptr)
    , quiet_(true)
{
	TrunkRadius_ = 0;
	TreeHeight_ = 0;
	BoundingDistance_ = 0;
	VecLeaves_.clear();
}


Skeleton::~Skeleton()
{
	if (Delaunay_)
		delete Delaunay_;
	if (MST_)
		delete MST_;
	if (FineGraph_)
		delete FineGraph_;
	if (KDtree_)
		delete KDtree_;
	if (Points_)
		delete Points_;
	if (VecLeaves_.size() > 0)
		VecLeaves_.clear();
}


bool Skeleton::build_delaunay(easy3d::PointCloud* cloud)
{
	//initialize
	if (Delaunay_)
		Delaunay_->clear();
	else
		Delaunay_ = new Graph;

	//read vertices into the graph
    if (!quiet_)
        std::cout << "read vertices into the delaunay..." << std::endl;
	int nPoints = cloud->n_vertices();
	easy3d::PointCloud::VertexProperty<easy3d::vec3> points = cloud->get_vertex_property<easy3d::vec3>("v:point");
    std::vector<Vector3D> newVertices = centralize_main_points(cloud);
	for (int i = 0; i < nPoints; i++)
	{
		SGraphVertexProp pV;
		pV.cVert = easy3d::vec3(newVertices[i].x, newVertices[i].y, newVertices[i].z);
		pV.nParent = 0;
		pV.lengthOfSubtree = 0.0;
		add_vertex(pV, *Delaunay_);
	}

	// Generate graph edges
    if (!quiet_)
        std::cout << "generate delaunay edges..." << std::endl;
	tetgenio tet_in, tet_out;
	tet_in.numberofpoints = nPoints;
	tet_in.pointlist = new REAL[tet_in.numberofpoints * 3];
	int count = 0;
	for (auto v : cloud->vertices())
	{
		tet_in.pointlist[count * 3 + 0] = points[v].x;
		tet_in.pointlist[count * 3 + 1] = points[v].y;
		tet_in.pointlist[count * 3 + 2] = points[v].z;
		++count;
	}
	const std::string str("");
	tetrahedralize(const_cast<char*>(str.c_str()), &tet_in, &tet_out);
	for (long nTet = 0; nTet < tet_out.numberoftetrahedra; nTet++) 
	{
		long tet_first = nTet * tet_out.numberofcorners;
		for (long i = tet_first; i < tet_first + tet_out.numberofcorners; i++) 
			for (long j = i + 1; j < tet_first + tet_out.numberofcorners; j++)
				add_edge(vertex(tet_out.tetrahedronlist[i], *Delaunay_), vertex(tet_out.tetrahedronlist[j], *Delaunay_), *Delaunay_);
	}

	//compute the weight of each edge
    if (!quiet_)
        std::cout << "compute Delaunay graph edges weights..." << std::endl;
    compute_delaunay_weight();

    if (!quiet_)
        std::cout << "finish the delaunay graph building!" << std::endl;
	return true;
}


bool Skeleton::extract_mst()
{
	//initialize
	if (MST_)
		MST_->clear();
	else
		MST_ = new Graph;

	//read vertices into the MST graph
    if (!quiet_)
        std::cout << "read vertices into the MST graph..." << std::endl;
	std::pair<SGraphVertexIterator, SGraphVertexIterator> vp = vertices(*Delaunay_);
	for (SGraphVertexIterator cIter = vp.first; cIter != vp.second; ++cIter)
	{
		SGraphVertexProp pV;
		pV.cVert = (*Delaunay_)[*cIter].cVert;
		pV.nParent = 0;
		pV.lengthOfSubtree = 0.0;
		add_vertex(pV, *MST_);
	}

	//extract the root vertex
    if (!quiet_)
        std::cout << "get the root vertex..." << std::endl;
    compute_root_vertex(MST_);

	//Find the spanning tree edges with minimum sum distance
    if (!quiet_)
        std::cout << "compute the shortest spanning tree..." << std::endl;
	std::vector<double> distances(num_vertices(*Delaunay_));
	std::vector<SGraphVertexDescriptor> vecParent(num_vertices(*Delaunay_));
	dijkstra_shortest_paths(*Delaunay_, RootV_, weight_map(get(&SGraphEdgeProp::nWeight, *Delaunay_))
		.distance_map(&distances[0])
		.predecessor_map(&(vecParent[0])));

	//Read the edges into the MST graph
	for (unsigned int nP = 0; nP < vecParent.size(); ++nP)
	{
		if (vertex(nP, *MST_) != vecParent.at(nP))
		{
			SGraphEdgeProp pEdge;
			pEdge.nWeight = 0.0;
			pEdge.nRadius = 0.0;
			pEdge.vecPoints.clear();
			add_edge(vertex(nP, *MST_), vertex(vecParent.at(nP), *MST_), pEdge, *MST_);
		}
		(*MST_)[vertex(nP, *MST_)].nParent = vecParent.at(nP);
	}

	//compute the length of subtree and the edges weights
    if (!quiet_)
        std::cout << "compute the subtree length for each vertex..." << std::endl;
    compute_length_of_subtree(MST_, RootV_);
    if (!quiet_)
        std::cout << "finish the minimum spanning tree extraction!" << std::endl;
	return true;
}


bool Skeleton::refine_skeleton()
{
    if (!quiet_)
        std::cout << "step 1: eliminate unimportant small edges" << std::endl;
    keep_main_skeleton(MST_, 0.019);

    if (!quiet_)
        std::cout << "step 2: iteratively merge collapsed edges" << std::endl;
    merge_collapsed_edges();

    if (!quiet_)
        std::cout << "finish the skeleton graph refining!" << std::endl;
	return true;
}


bool Skeleton::inflate_branches()
{
    if (!quiet_)
        std::cout << "step 1: assign points to corresponding branch edges" << std::endl;
    assign_points_to_edges();

    if (!quiet_)
        std::cout << "step 2: fit accurate radius to the trunk" << std::endl;
    fit_trunk();

    if (!quiet_)
        std::cout << "step 3: adjust the radius for all left branches" << std::endl;
    compute_all_edges_radius(TrunkRadius_);

    if (!quiet_)
        std::cout << "finish the branches inflation!" << std::endl;
	return true;
}


bool Skeleton::add_leaves()
{
    if (!quiet_)
        std::cout << "step 1: find leaf vertices in the tree graph" << std::endl;
    std::vector<SGraphVertexDescriptor> leafVertices = find_end_vertices();
	
    if (!quiet_)
        std::cout << "step 2: randomly generate leaves for each leaf vertex" << std::endl;
	//initialize
	std::size_t nLeaves = leafVertices.size();
	if (VecLeaves_.size() > 0)
		VecLeaves_.clear();
	//generate leaves for each leaf vertex
	for (std::size_t i = 0; i < nLeaves; i++)
	{
		SGraphVertexDescriptor currentLeafVertex = leafVertices.at(i);
        generate_leaves(currentLeafVertex, 0.05);
	}

    if (!quiet_)
        std::cout << "finish adding the leaves!" << std::endl;
	return true;
}


void Skeleton::keep_main_skeleton(Graph *i_Graph, double subtree_Threshold)
{
	//initialize
	if (FineGraph_)
		FineGraph_->clear();
	else
		FineGraph_ = new Graph;

	//read vertices into the fine graph
	std::pair<SGraphVertexIterator, SGraphVertexIterator> vp = vertices(*i_Graph);
	for (SGraphVertexIterator cIter = vp.first; cIter != vp.second; ++cIter)
	{
		SGraphVertexProp pV;
		pV.cVert = (*i_Graph)[*cIter].cVert;
		pV.nParent = (*i_Graph)[*cIter].nParent;
		pV.lengthOfSubtree = (*i_Graph)[*cIter].lengthOfSubtree;
		add_vertex(pV, *FineGraph_);
	}

	//read main edges with sufficient subtree length
	std::vector<SGraphVertexDescriptor> stack;
	stack.push_back(RootV_);
	while (true)
	{
		SGraphVertexDescriptor currentV = stack.back();
		stack.pop_back();
		std::pair<SGraphAdjacencyIterator, SGraphAdjacencyIterator> aj = adjacent_vertices(currentV, *i_Graph);
		for (SGraphAdjacencyIterator aIter = aj.first; aIter != aj.second; ++aIter)
		{
			if (*aIter != (*i_Graph)[currentV].nParent)
			{
				double child2Current = std::sqrt((*i_Graph)[currentV].cVert.distance2((*i_Graph)[*aIter].cVert));
				double subtreeRatio = ((*i_Graph)[*aIter].lengthOfSubtree + child2Current) / (*i_Graph)[currentV].lengthOfSubtree;
				if (subtreeRatio >= subtree_Threshold)
				{
					SGraphEdgeProp pEdge;
					SGraphEdgeDescriptor sEdge = edge(*aIter, currentV, (*i_Graph)).first;
					pEdge.nWeight = (*i_Graph)[sEdge].nWeight;
					pEdge.nRadius = (*i_Graph)[sEdge].nRadius;
					pEdge.vecPoints = (*i_Graph)[sEdge].vecPoints;
					SGraphVertexDescriptor dSource = source(sEdge, *i_Graph);
					SGraphVertexDescriptor dTarget = target(sEdge, *i_Graph);
					add_edge(dSource, dTarget, pEdge, *FineGraph_);
					stack.push_back(*aIter);
				}
			}
		}
		if (stack.size() == 0)
			break;
	}

	//update the length of subtree and weights for all vertices and edges
    compute_length_of_subtree(FineGraph_, RootV_);
    compute_graph_edges_weight(FineGraph_);
    compute_all_edges_radius(TrunkRadius_);
	return;
}


void Skeleton::merge_collapsed_edges()
{
	//start checking collapsed edges all over the graph
	std::pair<SGraphVertexIterator, SGraphVertexIterator> vp = vertices(*FineGraph_);
	bool bChange = true;
	int numComplex = 0;
	while (bChange)
	{
		bChange = false;
		for (SGraphVertexIterator cIter = vp.first; cIter != vp.second; ++cIter)
		{
			SGraphVertexDescriptor dVertex = *cIter;
			//if the current vertex has multiple children vertices
			if ((out_degree(dVertex, *FineGraph_) > 2) || 
			(((*FineGraph_)[dVertex].nParent == dVertex) && (out_degree(dVertex, *FineGraph_) > 1))) 
			{
                if (check_overlap_child_vertex(FineGraph_, dVertex))
				{
					bChange = true;
					numComplex++;
				}
			}
			//if the current vertex has only one single child vertex
			else if ((out_degree(dVertex, *FineGraph_) == 2) && 
			((*FineGraph_)[dVertex].nParent != dVertex))
			{
                if (check_single_child_vertex(FineGraph_, dVertex))
				{
					bChange = true;
					numComplex++;
				}
			}
		}
	}

	//update the length of subtree and weights for all vertices and edges
    compute_length_of_subtree(FineGraph_, RootV_);
    compute_graph_edges_weight(FineGraph_);
    compute_all_edges_radius(TrunkRadius_);
	return;
}


bool Skeleton::check_overlap_child_vertex(Graph* i_Graph, SGraphVertexDescriptor i_dVertex)
{
	//initialize
	double nMinMergeValue = DBL_MAX;
	std::vector<SGraphVertexDescriptor> vecChilds;
	SGraphVertexDescriptor sourceV;
	SGraphVertexDescriptor targetV;

	//find all children vertices of the input vertex and push them into the vector
	std::pair<SGraphOutEdgeIterator, SGraphOutEdgeIterator> listAdj = out_edges(i_dVertex, (*i_Graph));
	for (SGraphOutEdgeIterator cIter = listAdj.first; cIter != listAdj.second; ++cIter)
	{
		SGraphVertexDescriptor currentV;
		if (source(*cIter, *i_Graph) == i_dVertex) 
			currentV = target(*cIter, *i_Graph);
		else if (target(*cIter, *i_Graph) == i_dVertex)
			currentV = source(*cIter, *i_Graph);
		//if the current vertex is not the parent
		if ((*i_Graph)[currentV].nParent == i_dVertex) 
			vecChilds.push_back(currentV);
	}

	//traverse the children vertices and find all possible pairs
	for (int i = 0; i < vecChilds.size() - 1; i++)
	{
		for (int j = i + 1; j < vecChilds.size(); j++)
		{
			//get the current children pair and compute the merge value
			SGraphVertexDescriptor vi = vecChilds[i];
			SGraphVertexDescriptor vj = vecChilds[j];
            double merge_i2j = compute_merge_value(i_Graph, vi, vj);
            double merge_j2i = compute_merge_value(i_Graph, vj, vi);

			//check and identify the pair with least similarity value
			if (merge_i2j < merge_j2i && merge_i2j < nMinMergeValue)
			{
				nMinMergeValue = merge_i2j;
				sourceV = vi;
				targetV = vj;
			}
			else if (merge_j2i < merge_i2j && merge_j2i < nMinMergeValue)
			{
				nMinMergeValue = merge_j2i;
				sourceV = vj;
				targetV = vi;
			}
		}
	}

	//if the merge value is too large, then don't merge
	if (nMinMergeValue > 1.0) 
		return false;
	else
        return merge_vertices(i_Graph, sourceV, targetV, 0.5, 0.5);
}


bool Skeleton::check_single_child_vertex(Graph* i_Graph, SGraphVertexDescriptor i_dVertex)
{
	//find the only child of the current vertex
	SGraphVertexDescriptor childV;
	std::pair<SGraphOutEdgeIterator, SGraphOutEdgeIterator> listAdj = out_edges(i_dVertex, (*i_Graph));
	for (SGraphOutEdgeIterator cIter = listAdj.first; cIter != listAdj.second; ++cIter)
	{
		SGraphVertexDescriptor currentV;
		if (source(*cIter, *i_Graph) == i_dVertex)
			currentV = target(*cIter, *i_Graph);
		else if (target(*cIter, *i_Graph) == i_dVertex)
			currentV = source(*cIter, *i_Graph);
		//if the current vertex is not the parent
		if ((*i_Graph)[currentV].nParent == i_dVertex)
			childV = currentV;
	}

	//compute the distance between the current vertex and the line of its parent and child
	SGraphVertexDescriptor parentV = (*i_Graph)[i_dVertex].nParent;
	easy3d::vec3 pParent = (*i_Graph)[parentV].cVert;
	easy3d::vec3 pCurrent = (*i_Graph)[i_dVertex].cVert;
	easy3d::vec3 pChild = (*i_Graph)[childV].cVert;
	easy3d::vec3 pCross = easy3d::cross(pCurrent - pParent, pCurrent - pChild);
	double distance = pCross.length()/(pParent - pChild).length();

	//determine the merging threshold and check if current vertex can be merged or not
	double r = (*i_Graph)[edge(i_dVertex, parentV, *i_Graph).first].nRadius;
	if (distance >= 1.0 * r) 
		return false;
	else
	{
		//clear the current vertex
		(*i_Graph)[childV].nParent = parentV;
		(*i_Graph)[parentV].lengthOfSubtree = (*i_Graph)[childV].lengthOfSubtree + (pParent - pChild).length();
		clear_vertex(i_dVertex, *i_Graph);
		SGraphEdgeProp pEdge;
		pEdge.nWeight = ((*i_Graph)[childV].lengthOfSubtree + (*i_Graph)[parentV].lengthOfSubtree) / 2.0;
		pEdge.nRadius = r;
		add_edge(childV, parentV, pEdge, *i_Graph);
		return true;
	}
}


bool Skeleton::merge_vertices(Graph* i_Graph, SGraphVertexDescriptor i_dSource, SGraphVertexDescriptor i_dTarget, double i_wSource, double i_wTarget)
{
	//initialize
	std::set<SGraphVertexDescriptor> sGroupVertices;
	std::map<SGraphVertexDescriptor, double> mapAdjToRadius;
	std::pair<SGraphAdjacencyIterator, SGraphAdjacencyIterator> adjList;

	//put all vertices adjacent to source vertex or target vertex
	adjList = adjacent_vertices(i_dSource, *i_Graph);
	sGroupVertices.insert(adjList.first, adjList.second);
	adjList = adjacent_vertices(i_dTarget, *i_Graph);
	sGroupVertices.insert(adjList.first, adjList.second);

	//map the radius to the corresponding vertex
	for (std::set<SGraphVertexDescriptor>::const_iterator cIter = sGroupVertices.begin(); cIter != sGroupVertices.end(); ++cIter)
	{
		double mapRadius;
		//if the current vertex is connected to both the sourceV and targetV
		if ((edge(*cIter, i_dTarget, *i_Graph).second) && (edge(*cIter, i_dSource, *i_Graph).second))
		{
			double sourceRadius = (*i_Graph)[edge(*cIter, i_dSource, *i_Graph).first].nRadius;
			double targetRadius = (*i_Graph)[edge(*cIter, i_dTarget, *i_Graph).first].nRadius;
			mapRadius = max(sourceRadius, targetRadius);
		}
		//if the current vertex is connected only to the targetV
		else if (edge(*cIter, i_dTarget, *i_Graph).second)
		{
			mapRadius = (*i_Graph)[edge(*cIter, i_dTarget, *i_Graph).first].nRadius;
		}
		//if the current vertex is connected only to the sourceV
		else if (edge(*cIter, i_dSource, *i_Graph).second)
		{
			mapRadius = (*i_Graph)[edge(*cIter, i_dSource, *i_Graph).first].nRadius;
		}
		std::pair<SGraphVertexDescriptor, float> p(*cIter, static_cast<float>(mapRadius));
		mapAdjToRadius.insert(p);
	}

	//create a new vertex and merge two old vertices
	SGraphVertexProp pV;
	pV.nParent = (*i_Graph)[i_dTarget].nParent;
	easy3d::vec3 pSource, pTarget, pNew;
	pSource = (*i_Graph)[i_dSource].cVert;
	pTarget = (*i_Graph)[i_dTarget].cVert;
	pNew = i_wSource*pSource*(*i_Graph)[i_dSource].lengthOfSubtree + i_wTarget*pTarget*(*i_Graph)[i_dTarget].lengthOfSubtree;
	pNew = pNew / (i_wSource*(*i_Graph)[i_dSource].lengthOfSubtree + i_wTarget*(*i_Graph)[i_dTarget].lengthOfSubtree);
	pV.cVert = pNew;
	pV.lengthOfSubtree = max((*i_Graph)[i_dSource].lengthOfSubtree, (*i_Graph)[i_dTarget].lengthOfSubtree);

	// remove old vertices from cGraph
	clear_vertex(i_dSource, *i_Graph);
	clear_vertex(i_dTarget, *i_Graph);
	(*i_Graph)[i_dTarget] = pV;

	//update info for all adjacent vertices
	for (std::set<SGraphVertexDescriptor>::const_iterator cIter = sGroupVertices.begin(); cIter != sGroupVertices.end(); ++cIter)
	{
		if ((i_dSource != *cIter) && (i_dTarget != *cIter))
		{
			SGraphEdgeProp pEdge;
			pEdge.nRadius = mapAdjToRadius.find(*cIter)->second;
			add_edge(i_dTarget, *cIter, pEdge, *i_Graph);
			if (((*i_Graph)[*cIter].nParent == i_dSource) || ((*i_Graph)[*cIter].nParent == i_dTarget))
				(*i_Graph)[*cIter].nParent = i_dTarget;
			SGraphVertexDescriptor parentV = (*i_Graph)[*cIter].nParent;
			pEdge.nWeight = (pV.lengthOfSubtree + (*i_Graph)[parentV].lengthOfSubtree) / 2.0;
		}
	}

	return true;
}


void Skeleton::compute_delaunay_weight()
{
	//set the weight as the length of the edge 
	std::pair<SGraphEdgeIterator, SGraphEdgeIterator> ep = edges(*Delaunay_);
	SGraphVertexDescriptor dVertex1, dVertex2;
	easy3d::vec3 pVertex1, pVertex2;
	for (SGraphEdgeIterator cIter = ep.first; cIter != ep.second; ++cIter)
	{
		dVertex1 = source(*cIter, *Delaunay_);
		dVertex2 = target(*cIter, *Delaunay_);
		pVertex1 = (*Delaunay_)[dVertex1].cVert;
		pVertex2 = (*Delaunay_)[dVertex2].cVert;
		(*Delaunay_)[*cIter].nWeight = pVertex2.distance2(pVertex1);
	}
	return;
}


void Skeleton::compute_root_vertex(Graph* i_Graph)
{
	if (!i_Graph)
	{
		std::cout << "first create the graph!" << std::endl;
		return;
	}

	//the root vertex is set as the lowest vertex
	std::pair<SGraphVertexIterator, SGraphVertexIterator> vp = vertices(*i_Graph);
	SGraphVertexDescriptor initialVertex = *(vp.first);
	easy3d::vec3 pCurrent, pOther;
	for (SGraphVertexIterator cIter = vp.first; cIter != vp.second; ++cIter)
	{
		pCurrent = (*i_Graph)[initialVertex].cVert;
		pOther = (*i_Graph)[*cIter].cVert;
		if (pOther.z < pCurrent.z)
			initialVertex = *cIter;
	}

	RootV_ = initialVertex;
	return;
}


void Skeleton::compute_length_of_subtree(Graph* i_Graph, SGraphVertexDescriptor i_dVertex)
{
	(*i_Graph)[i_dVertex].lengthOfSubtree = 0.0;
	std::pair<SGraphAdjacencyIterator, SGraphAdjacencyIterator> adjacency = adjacent_vertices(i_dVertex, *i_Graph);
	for (SGraphAdjacencyIterator cIter = adjacency.first; cIter != adjacency.second; ++cIter)
	{
		if (*cIter != (*i_Graph)[i_dVertex].nParent)
		{
            compute_length_of_subtree(i_Graph, *cIter);
			easy3d::vec3 pChild = (*i_Graph)[*cIter].cVert;
			easy3d::vec3 pCurrent = (*i_Graph)[i_dVertex].cVert;
			double distance = std::sqrt(pCurrent.distance2(pChild));
			double child_Length = (*i_Graph)[*cIter].lengthOfSubtree + distance;
			if (i_Graph == MST_)
				(*i_Graph)[i_dVertex].lengthOfSubtree += child_Length;
			//for fine graph, a different way is used to compute the length to better represent the radius
			else if (i_Graph == FineGraph_)
			{
				if ((*i_Graph)[i_dVertex].lengthOfSubtree < child_Length)
					(*i_Graph)[i_dVertex].lengthOfSubtree = child_Length;
			}
		}
	}

	return;
}


void Skeleton::compute_graph_edges_weight(Graph* i_Graph)
{
	std::pair<SGraphEdgeIterator, SGraphEdgeIterator> ep = edges(*i_Graph);
	for (SGraphEdgeIterator eIter = ep.first; eIter != ep.second; ++eIter)
	{
		double subtreeWeight = (*i_Graph)[source(*eIter, *i_Graph)].lengthOfSubtree + (*i_Graph)[target(*eIter, *i_Graph)].lengthOfSubtree;
		(*i_Graph)[*eIter].nWeight = subtreeWeight / 2.0;
	}

	return;
}


void Skeleton::compute_all_edges_radius(double trunkRadius)
{
	//find the trunk edge
	SGraphEdgeDescriptor trunkE;
	std::pair<SGraphOutEdgeIterator, SGraphOutEdgeIterator> listAdj = out_edges(RootV_, (*FineGraph_));
	for (SGraphOutEdgeIterator eIter = listAdj.first; eIter != listAdj.second; ++eIter)
	{
		trunkE = *eIter;
		break;
	}

	//assign the radius to the rest branches
	double avrRadius = trunkRadius / pow((*FineGraph_)[trunkE].nWeight, 1.1);
	std::pair<SGraphEdgeIterator, SGraphEdgeIterator> ep = edges(*FineGraph_);
	for (SGraphEdgeIterator eIter = ep.first; eIter != ep.second; ++eIter)
	{
		(*FineGraph_)[*eIter].nRadius = pow((*FineGraph_)[*eIter].nWeight, 1.1) * avrRadius;
	}

	return;
}


double Skeleton::compute_merge_value(Graph* i_Graph, SGraphVertexDescriptor i_dSource, SGraphVertexDescriptor i_dTarget)
{
	//the input two vertices must share the same parent
	assert((*i_Graph)[i_dSource].nParent == (*i_Graph)[i_dTarget].nParent);

	//compute the merge value from source V to target V
	SGraphVertexDescriptor parentV = (*i_Graph)[i_dSource].nParent;
	easy3d::vec3 dirSource = (*i_Graph)[i_dSource].cVert - (*i_Graph)[parentV].cVert;
	easy3d::vec3 dirTarget = (*i_Graph)[i_dTarget].cVert - (*i_Graph)[parentV].cVert;
	double nLengthSource = dirSource.length();
	double nLengthTarget = dirTarget.length();
	dirSource.normalize();
	dirTarget.normalize();
	double alpha = easy3d::dot(dirSource, dirTarget);
	double nRadiusTarget = (*i_Graph)[edge(i_dTarget, parentV, (*i_Graph)).first].nRadius;
	
	//if the angle is smaller than 25 degrees
	if (alpha > 0.9)
		if(nLengthSource/nLengthTarget >= 0.5 && nLengthSource / nLengthTarget <= 2)
			return nLengthSource * sin(acos(alpha)) / nRadiusTarget;
	//if the angle is too large, then don't merge for this moment
    return DBL_MAX;
}


std::vector<Vector3D> Skeleton::centralize_main_points(easy3d::PointCloud* cloud)
{
    if (!quiet_)
        std::cout << "start centralizing the main-branch points" << std::endl;
	
	//retrive the points from the raw point cloud
	int nPt = cloud->n_vertices();
	if (Points_)
		delete Points_;
	Points_ = new Vector3D[nPt];
	easy3d::PointCloud::VertexProperty<easy3d::vec3> pts = cloud->get_vertex_property<easy3d::vec3>("v:point");
	int Count = 0;
	for (auto v : cloud->vertices())
	{
		Points_[Count].x = pts[v].x;
		Points_[Count].y = pts[v].y;
		Points_[Count].z = pts[v].z;
		Count++;
	}
	KDtree_ = new KdTree(Points_, nPt, 16);

	//compute the density of each point 
	std::vector<double> densityList;
	std::vector<Vector3D> vertices;
    obtain_initial_radius(cloud);
	for (int i = 0; i < nPt; i++)
	{
		Vector3D pCurrent = Points_[i];
		double density = 0.0;
		double distance = (pCurrent - RootPos_).normalize();
		double threshold = TrunkRadius_ * (1 - distance / BoundingDistance_); //get the query distance
		KDtree_->queryRange(pCurrent, threshold, true);
		int neighbourSize = KDtree_->getNOfFoundNeighbours();
		if (threshold != 0) 
			density = neighbourSize / threshold;
		densityList.push_back(density);
	}

	// for each point, check if it will be centralized or not
	double epsilon = 0.5;
	for (int j = 0; j < nPt; j++)
	{
		Vector3D pCurrent = Points_[j];
		double distance = (pCurrent - RootPos_).normalize();
		if (distance != 0) // the point is not the root
		{
			//the point doesn't lie far from the root
			if (distance < epsilon*BoundingDistance_) 
			{
				double ptDensity = densityList[j];
				double dendiff = 0.0;
				Vector3D pSum(0, 0, 0);
				double threshold = TrunkRadius_ * (1 - distance / BoundingDistance_);
				KDtree_->queryRange(pCurrent, threshold, true);
				int neighbourSize = KDtree_->getNOfFoundNeighbours();
				for (int np = 0; np < neighbourSize; np++)
				{
					int pointIndex = KDtree_->getNeighbourPositionIndex(np);
					double currentDensity = densityList[pointIndex];
					Vector3D pCurrent = Points_[pointIndex];
					pSum += pCurrent;
					dendiff = abs(currentDensity - ptDensity);
				}
				dendiff = dendiff / neighbourSize;
				pSum = pSum / neighbourSize;
				if (dendiff < 0.6)
				{
					vertices.push_back(pSum);
					continue;
				}
			}
		}
		vertices.push_back(pCurrent);
	}

	return vertices;
}


void Skeleton::obtain_initial_radius(easy3d::PointCloud* cloud)
{
	//get the lowest root point from the point cloud
	easy3d::vec3 pLowest(0, 0, FLT_MAX), pOther;
	easy3d::PointCloud::VertexProperty<easy3d::vec3> points = cloud->get_vertex_property<easy3d::vec3>("v:point");
	for (auto v : cloud->vertices())
	{
		pOther = points[v];
		if (pOther.z < pLowest.z)
			pLowest = pOther;
	}
	RootPos_.x = pLowest.x;
	RootPos_.y = pLowest.y;
	RootPos_.z = pLowest.z;
    if (!quiet_)
        std::cout << "the root vertex coordinate is:" << std::endl;
    if (!quiet_)
        std::cout << RootPos_.x << " " << RootPos_.y << " " << RootPos_.z << "\n" << std::endl;

	//get the tree height and the bounding distance
	for (auto v : cloud->vertices())
	{
		pOther = points[v];
		if ((pOther.z - pLowest.z) > TreeHeight_)
			TreeHeight_ = pOther.z - pLowest.z;
		if (std::sqrt(pOther.distance2(pLowest)) > BoundingDistance_)
			BoundingDistance_ = std::sqrt(pOther.distance2(pLowest));
	}

	//get all points that lie within 2% of the tree height
	std::vector<easy3d::vec3> trunkList;
	double epsiony = 0.02;
	for (auto v : cloud->vertices())
	{
		pOther = points[v];
		if ((pOther.z - pLowest.z) <= epsiony * TreeHeight_)
			trunkList.push_back(pOther);
	}

	//project the trunk points on the xy plane and get the bounding box
	double minX = DBL_MAX;
	double maxX = -DBL_MAX;
	double minY = DBL_MAX;
	double maxY = -DBL_MAX;
	for (int nP = 0; nP < trunkList.size(); nP++)
	{
		if (minX > trunkList[nP].x)
			minX = trunkList[nP].x;
		if (maxX < trunkList[nP].x)
			maxX = trunkList[nP].x;
		if (minY > trunkList[nP].y)
			minY = trunkList[nP].y;
		if (maxY < trunkList[nP].y)
			maxY = trunkList[nP].y;
	}

	//assign the raw radius value and return
	TrunkRadius_ = max((maxX - minX), (maxY - minY)) / 2.0;
    if (!quiet_)
        std::cout << "the initial radius is:" << std::endl;
    if (!quiet_)
        std::cout << TrunkRadius_ << "\n" << std::endl;
	return;
}


void Skeleton::assign_points_to_edges()
{
	//check if the kd tree has been built
	if (!KDtree_)
	{
        if (!quiet_)
            std::cout << "wrong in KD tree construction!" << std::endl;
		return;
	}

	//for each edge, find its corresponding points
	SGraphEdgeDescriptor currentE;
	std::pair<SGraphEdgeIterator, SGraphEdgeIterator> ep = edges(*FineGraph_);
	for (SGraphEdgeIterator eIter = ep.first; eIter != ep.second; ++eIter)
	{
		//extract two end vertices of the current edge
		currentE = *eIter;
		(*FineGraph_)[currentE].vecPoints.clear();
		double currentR = (*FineGraph_)[currentE].nRadius;
		SGraphVertexDescriptor sourceV, targetV;
		if (source(currentE, *FineGraph_) == (*FineGraph_)[target(currentE, *FineGraph_)].nParent)
		{
			sourceV = source(currentE, *FineGraph_);
			targetV = target(currentE, *FineGraph_);
		}
		else
		{
			sourceV = target(currentE, *FineGraph_);
			targetV = source(currentE, *FineGraph_);
		}
		Vector3D pSource((*FineGraph_)[sourceV].cVert.x, (*FineGraph_)[sourceV].cVert.y, (*FineGraph_)[sourceV].cVert.z);
		Vector3D pTarget((*FineGraph_)[targetV].cVert.x, (*FineGraph_)[targetV].cVert.y, (*FineGraph_)[targetV].cVert.z);
		//query neighbor points from the kd tree
		KDtree_->queryLineIntersection(pSource, pTarget, 3.0 * currentR, true, true);
		int neighbourSize = KDtree_->getNOfFoundNeighbours();
		for (int i = 0; i < neighbourSize; i++)
		{
			//get the current neighbor point and check if it lies within the cylinder
			int ptIndex = KDtree_->getNeighbourPositionIndex(i);
			Vector3D pCurrent = Points_[ptIndex];
			Vector3D cDirPoint = pCurrent - pSource;
			Vector3D cDirCylinder = pTarget - pSource;
			double nLengthPoint = cDirPoint.normalize();
			double nLengthCylinder = cDirCylinder.normalize();
			double cosAlpha = Vector3D::dotProduct(cDirCylinder, cDirPoint);
			//if the angle is smaller than 90 and the projection is less than the axis length
			if (cosAlpha >= 0 && nLengthPoint * cosAlpha <= nLengthCylinder)
				(*FineGraph_)[currentE].vecPoints.push_back(ptIndex);
		}
	}

	return;
}


void Skeleton::fit_trunk()
{
	//find the trunk edge
	SGraphEdgeDescriptor trunkE;
	std::pair<SGraphOutEdgeIterator, SGraphOutEdgeIterator> listAdj = out_edges(RootV_, (*FineGraph_));
	for (SGraphOutEdgeIterator eIter = listAdj.first; eIter != listAdj.second; ++eIter)
	{
		trunkE = *eIter;
		break;
	}

	//if the points attached are not enough, then don't conduct fitting
	std::size_t edgePoints = (*FineGraph_)[trunkE].vecPoints.size();
	if (edgePoints <= 20)
	{
        if (!quiet_)
            std::cout << "the least squares fails because of not enough points!" << std::endl;
		return;
	}

	//construct the initial cylinder
	SGraphVertexDescriptor sourceV, targetV;
	if (source(trunkE, *FineGraph_) == (*FineGraph_)[target(trunkE, *FineGraph_)].nParent)
	{
		sourceV = source(trunkE, *FineGraph_);
		targetV = target(trunkE, *FineGraph_);
	}
	else
	{
		sourceV = target(trunkE, *FineGraph_);
		targetV = source(trunkE, *FineGraph_);
	}
	Vector3D pSource((*FineGraph_)[sourceV].cVert.x, (*FineGraph_)[sourceV].cVert.y, (*FineGraph_)[sourceV].cVert.z);
	Vector3D pTarget((*FineGraph_)[targetV].cVert.x, (*FineGraph_)[targetV].cVert.y, (*FineGraph_)[targetV].cVert.z);
	Cylinder currentC = Cylinder(pSource, pTarget, (*FineGraph_)[trunkE].nRadius);

	//extract the corresponding point cloud
	std::vector<std::vector<double>> ptlist;
	for (int np = 0; np < edgePoints; np++)
	{
		int npIndex = (*FineGraph_)[trunkE].vecPoints.at(np);
		Vector3D pt = Points_[npIndex];
		std::vector<double> ptemp;
		ptemp.push_back(pt.x);
		ptemp.push_back(pt.y);
		ptemp.push_back(pt.z);
		ptemp.push_back(1.0); //weights are set to 1
		ptlist.push_back(ptemp);
	}

	//non linear leastsquares adjustment
	if (currentC.LeastSquaresFit(ptlist.begin(), ptlist.end())) 
	{
		Vector3D pSourceAdjust = currentC.GetAxisPosition1();
		Vector3D pTargetAdjust = currentC.GetAxisPosition2();
		double radiusAdjust = currentC.GetRadius();

		//prepare for the weighted non linear least squares
		currentC.SetAxisPosition1(pSourceAdjust);
		currentC.SetAxisPosition2(pTargetAdjust);
		currentC.SetRadius(radiusAdjust);
		double maxDis = -DBL_MAX;
		std::vector<double> disList;
		for (int np = 0; np < edgePoints; np++)
		{
			Vector3D pt(ptlist[np][0], ptlist[np][1], ptlist[np][2]);
			//Compute the distance from current pt to the line formed by source and target vertex
			double dis = (Vector3D::crossProduct(pt - pSourceAdjust, pt - pTargetAdjust)).normalize()
				         / ((pSourceAdjust - pTargetAdjust).normalize());
			//Substract the distance with the radius
			dis = abs(dis - radiusAdjust);
			if (dis > maxDis) maxDis = dis;
			disList.push_back(dis);
		}
		
		//update the weights
		for (int np = 0; np < edgePoints; np++)
			ptlist[np][3] = 1.0 - disList[np] / maxDis;
		
		//conduct the second round of weighted least squares
		if (currentC.LeastSquaresFit(ptlist.begin(), ptlist.end()))
		{
            if (!quiet_)
                std::cout << "successfully conduct the non linear least squares!" << std::endl;
			pSourceAdjust = currentC.GetAxisPosition1();
			pTargetAdjust = currentC.GetAxisPosition2();
			radiusAdjust = currentC.GetRadius();
		}

		// truncate the new cylinder to the points
		Vector3D axis = pTargetAdjust - pSourceAdjust;
		Vector3D cDirSource = pSource - pSourceAdjust;
		Vector3D cDirTarget = pTarget - pSourceAdjust;
		axis.normalize();
		float nLengthSource = cDirSource.normalize();
		float nLengthTarget = cDirTarget.normalize();
		double alphaSource = Vector3D::dotProduct(axis, cDirSource);
		double alphaTarget = Vector3D::dotProduct(axis, cDirTarget);
		Vector3D pSourceNew = pSourceAdjust + nLengthSource * alphaSource*axis;
		Vector3D pTargetNew = pSourceAdjust + nLengthTarget * alphaTarget*axis;

		(*FineGraph_)[sourceV].cVert = easy3d::vec3(pSourceNew.x, pSourceNew.y, pSourceNew.z);
		(*FineGraph_)[targetV].cVert = easy3d::vec3(pTargetNew.x, pTargetNew.y, pTargetNew.z);
		(*FineGraph_)[trunkE].nRadius = radiusAdjust;
		TrunkRadius_ = radiusAdjust;
		return;
	}
	else
	{
        if (!quiet_)
            std::cout << "the non linear least squares is unsuccessful!" << std::endl;
		return;
	}
}


std::vector<SGraphVertexDescriptor> Skeleton::find_end_vertices()
{
	std::vector<SGraphVertexDescriptor> endVertices;
	// retrieve all leaf vertices at the end of the tree graph
	std::pair<SGraphVertexIterator, SGraphVertexIterator> vp = vertices(*FineGraph_);
	for (SGraphVertexIterator cIter = vp.first; cIter != vp.second; ++cIter)
	{
		if (out_degree(*cIter, *FineGraph_) == 1)
		{
			if (*cIter != (*FineGraph_)[*cIter].nParent)
				endVertices.push_back(*cIter);
		}
	}

	return endVertices;
}


void Skeleton::generate_leaves(SGraphVertexDescriptor i_LeafVertex, double leafsize_Factor)
{
	//generate a random density number
	int density = ceil(Rand_0_1 * 10);
	double radius = 0.2 / log((float)num_edges(*FineGraph_));
	//get the position of the current leaf vertex and its parent
	easy3d::vec3 pCurrent = (*FineGraph_)[i_LeafVertex].cVert;
	SGraphVertexDescriptor i_LeafParent = (*FineGraph_)[i_LeafVertex].nParent;
	easy3d::vec3 pParent = (*FineGraph_)[i_LeafParent].cVert;
	//get the end position where the leaf should grow
	easy3d::vec3 pEnd = pCurrent - (Rand_0_1 / 2.0) * ((pCurrent - pParent).normalize());

	//generate i-th random leaf
	for (int i = 0; i < density; ++i)
	{
		//generate a random leaf position
		easy3d::vec3 dirLeaf((Rand_0_1 - 0.5) / 0.5, (Rand_0_1 - 0.5) / 0.5, (Rand_0_1 - 0.5) / 0.5);
		dirLeaf = dirLeaf.normalize();
		double l = Rand_0_1 * radius;
		easy3d::vec3 pLeaf = pEnd + dirLeaf * l;
		//generate normal and color vector
		easy3d::vec3 dirParent2Leaf = (pLeaf - pParent).normalize();
		easy3d::vec3 normal = (easy3d::cross(dirParent2Leaf, dirLeaf)).normalize();
		//generate a new leaf
		Leaf newleaf;
		newleaf.cPos = pLeaf;
		newleaf.cDir = dirLeaf;
		//generate a random normal vector direction
		easy3d::vec3 delta((Rand_0_1 - 0.5) / 0.5, (Rand_0_1 - 0.5) / 0.5, (Rand_0_1 - 0.5) / 0.5);
		newleaf.cNormal = (normal + Rand_0_1*delta*0.5).normalize();
		newleaf.pSource = i_LeafVertex;
		newleaf.nLength = BoundingDistance_ * leafsize_Factor;
		newleaf.nRad = newleaf.nLength / 5;
		VecLeaves_.push_back(newleaf);
	}

	return;
}


void Skeleton::get_graph_for_smooth(std::vector<Path> &pathList)
{
	pathList.clear();
	Path currentPath;
	int cursor = 0;
	//insert the root vertex to the current path
	currentPath.push_back(RootV_);
	pathList.push_back(currentPath);
	//retrieve the path list
	while (cursor < pathList.size())
	{
		currentPath = pathList[cursor];
		SGraphVertexDescriptor endV = currentPath.back();
		// if the current path has reached the leaf
		if ((out_degree(endV, *FineGraph_) == 1) && (endV != (*FineGraph_)[endV].nParent))
			cursor++;
		else
		{
			//find the fatest child vertex
			double maxR = -1;
			int isUsed = -1;
			SGraphVertexDescriptor fatestChild;
			std::vector<SGraphVertexDescriptor> notFastestChildren;
			std::pair<SGraphAdjacencyIterator, SGraphAdjacencyIterator> adjacencies = adjacent_vertices(endV, *FineGraph_);
			for (SGraphAdjacencyIterator cIter = adjacencies.first; cIter != adjacencies.second; ++cIter)
			{
				if (*cIter != (*FineGraph_)[endV].nParent)
				{
					SGraphEdgeDescriptor currentE = edge(endV, *cIter, *FineGraph_).first;
					double radius = (*FineGraph_)[currentE].nRadius;
					if (maxR < radius)
					{
						maxR = radius;
						if (isUsed > -1)
							notFastestChildren.push_back(fatestChild);
						else
							isUsed = 0;
						fatestChild = *cIter;
					}
					else
						notFastestChildren.push_back(*cIter);
				}
			}
			// organize children vertices into a new path
			for (int nChild = 0; nChild < notFastestChildren.size(); ++nChild)
			{
				Path newPath;
				newPath.push_back(endV);
				newPath.push_back(notFastestChildren[nChild]);
				pathList.push_back(newPath);
			}
			//put the fatest children into the path list
			pathList[cursor].push_back(fatestChild);
		}
	}
	
	return;
}

