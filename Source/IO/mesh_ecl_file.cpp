#ifdef _MSC_VER //kill some warnings
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "inmost.h"
#include "../Mesh/incident_matrix.hpp"
#if defined(USE_MESH)

// coords/zcorn algorithm
// 1) put all block nodes into pillars, sort each pillar nodes by actual depth
// 2) create edges down along pillars and mark them according to blocks
// 3) consider all pairs of pillars in Oxz and Oyz planes and create uncut block edges, mark them in block numbers
// 4) use line-sweep algorithm to intersect and cut all the edges between each pair of pillars, mark them in block numbers
// 5) mark all the nodes with block numbers that they belong by considering union of block numbers on adjacent edges
// 5) use incident_matrix algorithm to create all the faces between pair of pillars from pillar edges and inter-pillar edges
// 6) from intersection of block numbers on nodes figure out which blocks the face belongs
// 7) add top and bottom interface

//eclipse states
#define ECLSTRCMP(x,y) strncmp(x,y,8)

#define ECL_NEVER -1
#define ECL_NONE 0
#define ECL_SKIP_SECTION 1
#define ECL_INCLUDE 2
#define ECL_DIMENS 3
#define ECL_DX 4
#define ECL_DY 5
#define ECL_DZ 6
#define ECL_TOPS 7
#define ECL_PERMX 8
#define ECL_PERMY 9
#define ECL_PERMZ 10
#define ECL_PORO 11
#define ECL_MAPAXIS 12
#define ECL_INRAD 13
#define ECL_COORDS 14
#define ECL_ZCORN 15
#define ECL_ACTNUM 16

#define ECL_GTYPE_NONE 0
#define ECL_GTYPE_TOPS 1
#define ECL_GTYPE_ZCORN 2
#define ECL_GTYPE_RADIAL 3
#define ECL_GTYPE_CARTESIAN 4


#define ECL_VAR_NONE 0
#define ECL_VAR_REAL 1
#define ECL_VAR_INT 2

#define ECL_IJK_DATA(i,j,k) (i + ((j)+(k)*dims[1])*dims[0])

//line sweep events
#define SEG_START 0
#define SEG_END 1



namespace INMOST
{
	//special structure for array of 3 reals for std::map
	class position
	{
	public:
		Storage::real xyz[3];
		position & operator =(position const & b) {memcpy(xyz,b.xyz,sizeof(Storage::real)*3); return *this;}
		position() {memset(xyz,0,sizeof(Storage::real)*3);}
		position(Storage::real _xyz[3])  {memcpy(xyz,_xyz,sizeof(Storage::real)*3);}
		position(const position & b) {memcpy(xyz,b.xyz,sizeof(Storage::real)*3);}
		operator Storage::real *() {return xyz;}
		Storage::real & operator [](int k){return xyz[k];}
		Storage::real operator [](int k) const{return xyz[k];}
	};
	//2d point with comparison operator for line sweep algorithm
	struct Point
	{
		double x, y;
		Point & operator = (Point const & b) {  x = b.x; y = b.y; return *this; }
		Point(const Point & b) : x(b.x), y(b.y) {}
		Point(double _x, double _y) : x(_x), y(_y) {}
		bool operator <(const Point & b) const
		{
			if (y < b.y - 1.0e-9) return true;
			else if (y > b.y + 1.0e-9) return false;
			else if (x < b.x - 1.0e-9) return true;
			else return false;
		}
		bool operator ==(const Point & b) const
		{
			return fabs(y - b.y) < 1.0e-9 && fabs(x - b.x) < 1.0e-9;
		}
		bool operator !=(const Point & b) const
		{
			return fabs(y - b.y) > 1.0e-9 || fabs(x - b.x) > 1.0e-9;
		}
	};
	//Comparator for map of nodes
	class position_less
	{
	public:
		bool operator()(const position & a, const position & b) const
		{
			for(int k = 0; k < 3; ++k)
			{
				if( a[k] < b[k] - 1.0e-6)
					return true;
				else if( a[k] > b[k] + 1.0e-6 )
					return false;
			}
			return false;
		}
	};
	//Comparator for events in line sweep algorithm
	class event_less
	{
	public:
		bool operator()(const std::pair<double, int> & a, const std::pair<double, int> & b) const
		{
			if (a.first < b.first - 1.0e-6)
				return true;
			else if (a.first > b.first + 1.0e-6)
				return false;
			else if (a.second < b.second)
				return true;
			return false;
		}
	};
	class index_comparator
	{
		Storage::integer_array & data;
	public:
		index_comparator(Storage::integer_array & data) : data(data) {}
		index_comparator(const index_comparator & b) : data(b.data) {}
		index_comparator & operator =(index_comparator const & b) {data = b.data; return *this;}
		bool operator ()(int a, int b) const {return data[a] < data[b];}
	};
	template<typename T>
	int count_duplicates(ElementArray<T> & array)
	{
		Mesh * m = array.GetMeshLink();
		MarkerType mrk = m->CreateMarker();
		int dups = 0;
		for(int k = 0; k < array.size(); ++k)
		{
			if( array[k].GetMarker(mrk) )
				dups++;
			array[k].SetMarker(mrk);
		}
		array.RemMarker(mrk);
		m->ReleaseMarker(mrk);
		return dups;
	}
	
	template<typename T>
	void make_unique(ElementArray<T> & array)
	{
		Mesh * m = array.GetMeshLink();
		MarkerType mrk = m->CreateMarker();
		int i = 0, j = 0;
		while(j < array.size())
		{
			if( array[j].GetMarker(mrk) )
				++j;
			else
			{
				array[j].SetMarker(mrk);
				array[i++] = array[j++];
			}
		}
		array.RemMarker(mrk);
		array.resize(i);
		m->ReleaseMarker(mrk);
	}
	
	

	__INLINE void normalize(Storage::real v[3])
	{
		Storage::real l = sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
		if( l )
		{
			v[0] /= l;
			v[1] /= l;
			v[2] /= l;
		}
	}
	
	void compute_conormal(const Edge & a, const Edge & b, Storage::real vn[3])
	{
		Storage::real vz[3] = {0,0,1}, va[3], vb[3], na[3], nb[3], nn[3];
		Storage::real_array abeg = a->getBeg()->Coords();
		Storage::real_array aend = a->getEnd()->Coords();
		Storage::real_array bbeg = b->getBeg()->Coords();
		Storage::real_array bend = b->getEnd()->Coords();
		make_vec(abeg.data(),aend.data(),va);
		make_vec(bbeg.data(),bend.data(),vb);
		cross_prod(va,vz,na);
		cross_prod(vb,vz,nb);
		nn[0] = va[0]+vb[0];
		nn[1] = va[1]+vb[1];
		nn[2] = va[2]+vb[2];
		cross_prod(nn,vz,vn);
		normalize(vn);
	}
	
	//intersect a pair of segments
	/*
	std::pair<bool,Node> intersect_segments(Mesh * m, const Edge & a, const Edge & b, std::map<position,Node,position_less> & intersections, double & t1, double & t2, bool print)
	{
		if( a->getBeg() == b->getBeg() || a->getBeg() == b->getEnd() || a->getEnd() == b->getEnd() || a->getEnd() == b->getBeg() )
			return std::make_pair(false,InvalidNode());
		Storage::real_array abeg = a->getBeg()->Coords();
		Storage::real_array aend = a->getEnd()->Coords();
		Storage::real_array bbeg = b->getBeg()->Coords();
		Storage::real_array bend = b->getEnd()->Coords();
		Storage::real vn[3];
		compute_conormal(a,b,vn);
		Storage::real abeg_c_ = dot_prod(abeg.data(),vn);
		Storage::real aend_c_ = dot_prod(aend.data(),vn);
		Storage::real bbeg_c_ = dot_prod(bbeg.data(),vn);
		Storage::real bend_c_ = dot_prod(bend.data(),vn);
		position find;
		Storage::real div = (abeg[2] - aend[2])*(bbeg_c_ - bend_c_) - (abeg_c_ - aend_c_)*(bbeg[2] - bend[2]), find_c_;//, t1,t2;
		if (fabs(div) < 1.0e-13)
		{
			if (print) std::cout << "divisor is zero" << std::endl;
			return std::make_pair(false,InvalidNode());
		}
		find[2] = ((abeg[2]*aend_c_ - abeg_c_*aend[2])*(bbeg[2] - bend[2]) - (abeg[2] - aend[2])*(bbeg[2]*bend_c_ - bbeg_c_*bend[2])) / div;
		find_c_ = ((abeg[2]*aend_c_ - abeg_c_*aend[2])*(bbeg_c_ - bend_c_) - (abeg_c_ - aend_c_)*(bbeg[2]*bend_c_ - bbeg_c_*bend[2])) / div;
		if (print) std::cout << "found (c "  << find_c_ << ",z " << find[2] << ")" << std::endl;
		//probably some of these tests are redundant
		if (fabs(aend[2] - abeg[2]) > 1.0e-9)
		{
			t1 = (find[2] - abeg[2]) / (aend[2] - abeg[2]);
			if (t1 < 1.0e-9 || t1 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t1 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		if (fabs(aend_c_ - abeg_c_) > 1.0e-9)
		{
			t1 = (find_c_ - abeg_c_) / (aend_c_ - abeg_c_);
			if (t1 < 1.0e-9 || t1 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t1 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		if (fabs(bend[2] - bbeg[2]) > 1.0e-9)
		{
			t2 = (find[2] - bbeg[2]) / (bend[2] - bbeg[2]);
			if (t2 < 1.0e-9 || t2 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t2 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		if (fabs(bend_c_ - bbeg_c_) > 1.0e-9)
		{
			t2 = (find_c_ - bbeg_c_) / (bend_c_ - bbeg_c_);
			if (t2 < 1.0e-9 || t2 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t2 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		//restore third coordinate
		find[0] = 0.5*((1-t1)*abeg[0]+t1*aend[0] + (1-t2)*bbeg[0]+t2*bend[0]);
		find[0] = 0.5*((1-t1)*abeg[1]+t1*aend[1] + (1-t2)*bbeg[1]+t2*bend[1]);
		if (print) std::cout << "intersection accepted (" << find[0] << "," << find[1] << "," << find[2] << ")" << std::endl;
		Node I;
		std::map<position,Node,position_less>::iterator search = intersections.find(find);
		//check whether intersection already exists
		if( search != intersections.end() ) //there is a node!
			I = search->second;
		else //no node, create one
		{
			I = m->CreateNode(find);
			intersections.insert(std::make_pair(find,I));
		}
		return std::make_pair(true,I);
	}
	 */
	
	Point project(Storage::real pos[3], const Storage::real p0beg[3], const Storage::real  p0end[3], const Storage::real p1beg[3], const Storage::real p1end[3])
	{
		Storage::real alpha, beta, c;
		Storage::real v[3], v0[3], v1[3];
		alpha = (pos[2]-p0end[2])/(p0beg[2]-p0end[2]);
		beta = (pos[2]-p1end[2])/(p1beg[2]-p1end[2]);
		//get slice of pillar at z position of beginning of the segment
		v0[0] = p0end[0] + alpha*(p0beg[0]-p0end[0]);
		v0[1] = p0end[1] + alpha*(p0beg[1]-p0end[1]);
		v0[2] = pos[2];
		v1[0] = p1end[0] + alpha*(p1beg[0]-p1end[0]);
		v1[1] = p1end[1] + alpha*(p1beg[1]-p1end[1]);
		v1[2] = pos[2];
		//get vector connecting pillars
		make_vec(v1,v0,v);
		normalize(v);
		//project coordinates
		c = dot_prod(pos,v);
		return Point(pos[2],c);
	}
	
	//intersect a pair of segments
	std::pair<bool,Node> intersect_segments(Mesh * m, const Edge & a, const Edge & b, std::map<position,Node,position_less> & intersections, double & t1, double & t2,   const Storage::real p0beg[3], const Storage::real  p0end[3], const Storage::real p1beg[3], const Storage::real p1end[3], bool print)
	{
		if( a->getBeg() == b->getBeg() || a->getBeg() == b->getEnd() || a->getEnd() == b->getEnd() || a->getEnd() == b->getBeg() )
			return std::make_pair(false,InvalidNode());
		Storage::real_array abeg = a->getBeg()->Coords();
		Storage::real_array aend = a->getEnd()->Coords();
		Storage::real_array bbeg = b->getBeg()->Coords();
		Storage::real_array bend = b->getEnd()->Coords();
		Point pabeg = project(abeg.data(),p0beg,p0end,p1beg,p1end);
		Point paend = project(aend.data(),p0beg,p0end,p1beg,p1end);
		Point pbbeg = project(bbeg.data(),p0beg,p0end,p1beg,p1end);
		Point pbend = project(bend.data(),p0beg,p0end,p1beg,p1end);
		Point pfind(0,0);
		position find;
		Storage::real div = (pabeg.x - paend.x)*(pbbeg.y - pbend.y) - (pabeg.y - paend.y)*(pbbeg.x - pbend.x);
		if (fabs(div) < 1.0e-13)
		{
			if (print) std::cout << "divisor is zero" << std::endl;
			return std::make_pair(false,InvalidNode());
		}
		pfind.x = ((pabeg.x*paend.y - pabeg.y*paend.x)*(pbbeg.x - pbend.x) - (pabeg.x - paend.x)*(pbbeg.x*pbend.y - pbbeg.y*pbend.x)) / div;
		pfind.y = ((pabeg.x*paend.y - pabeg.y*paend.x)*(pbbeg.y - pbend.y) - (pabeg.y - paend.y)*(pbbeg.x*pbend.y - pbbeg.y*pbend.x)) / div;
		if (print) std::cout << "found ("<< pfind.x << ", " << pfind.y << ")" << std::endl;
		//probably some of these tests are redundant
		if (fabs(paend.x - pabeg.x) > 1.0e-9)
		{
			t1 = (pfind.x - pabeg.x) / (paend.x - pabeg.x);
			if (t1 < 1.0e-9 || t1 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t1 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		if (fabs(paend.y - pabeg.y) > 1.0e-9)
		{
			t1 = (pfind.y - pabeg.y) / (paend.y - pabeg.y);
			if (t1 < 1.0e-9 || t1 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t1 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		if (fabs(pbend.x - pbbeg.x) > 1.0e-9)
		{
			t2 = (pfind.x - pbbeg.x) / (pbend.x - pbbeg.x);
			if (t2 < 1.0e-9 || t2 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t2 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		if (fabs(pbend.y - pbbeg.y) > 1.0e-9)
		{
			t2 = (pfind.y - pbbeg.y) / (pbend.y - pbbeg.y);
			if (t2 < 1.0e-9 || t2 > 1.0 - 1.0e-9)  { if (print) std::cout << "out of bound: " << t2 << std::endl; return std::make_pair(false,InvalidNode()); }
		}
		//restore third coordinate
		for(int k = 0; k < 3; ++k)
			find[k] = 0.5*((1-t1)*abeg[k]+t1*aend[k] + (1-t2)*bbeg[k]+t2*bend[k]);
		if (print) std::cout << "intersection accepted (" << find[0] << "," << find[1] << "," << find[2] << ")" << std::endl;
		Node I;
		std::map<position,Node,position_less>::iterator search = intersections.find(find);
		//check whether intersection already exists
		if( search != intersections.end() ) //there is a node!
			I = search->second;
		else //no node, create one
		{
			I = m->CreateNode(find);
			intersections.insert(std::make_pair(find,I));
		}
		if( I == a->getBeg() || I == b->getBeg() || I == a->getEnd() || I == b->getEnd())
			return std::make_pair(false,I);
		return std::make_pair(true,I);
	}

	 
	void check_multimap(std::multimap<std::pair<double,int>, int,event_less> & events, const std::multimap<std::pair<double,int>, int,event_less>::iterator & it, bool print)
	{
		if( print )
		{
			{
				std::multimap<std::pair<double,int>, int,event_less>::iterator jt = it;
				++jt;
				if( jt != events.end() )
				{
					std::cout << "inserted " << std::scientific << it->first.first << " " << it->first.second << " segment " << it->second << std::endl;
					std::cout << "next     " << std::scientific << jt->first.first << " " << jt->first.second << " segment " << jt->second << std::endl;
					std::cout << "compare " << event_less()(it->first,jt->first) << std::endl;
				}
			}
			if( it != events.begin() )
			{
				std::multimap<std::pair<double,int>, int,event_less>::iterator  jt = it;
				--jt;
				std::cout << "prev     " << std::scientific << jt->first.first << " " << jt->first.second << " segment " << jt->second << std::endl;
				std::cout << "inserted " << std::scientific << it->first.first << " " << it->first.second << " segment " << it->second << std::endl;
				std::cout << "compare " << event_less()(jt->first,it->first) << std::endl;
			}
		}
	}
	//account for intersection event
	void intersect_event(Mesh * m, int a, int b, Node I, ElementArray<Edge> & segments, std::multimap<Point, int> & sweep, std::multimap<std::pair<double,int>, int,event_less> & events, std::vector<Tag> & transfer, bool print)
	{
		const bool checkmm = false;
		//remove event of ending of old segment
		{
			int rem_end_events[2];
			rem_end_events[0] = a;
			rem_end_events[1] = b;
			for (int k = 0; k < 2; ++k)
			{
				std::pair< std::multimap<std::pair<double,int>, int,event_less>::iterator, std::multimap<std::pair<double,int>,int,event_less>::iterator > del = events.equal_range(std::make_pair(segments[rem_end_events[k]]->getEnd()->Coords()[2],SEG_END)); //get all events at position of the end
				bool flag = false;
				for (std::multimap<std::pair<double,int>, int,event_less>::iterator it = del.first; it != del.second; ++it) //search over all events
				{
					if (it->first.second == SEG_END && it->second == rem_end_events[k]) //event is end of segment and segment matches current
					{
						events.erase(it); //remove that segment
						flag = true;
						break; //do not expect any more
					}
				}
				if (!flag)
				{
					std::cout << "Cannot find proper ending event for segment " << rem_end_events[k] << std::endl;
					std::cout << "Found events:" << std::endl;
					for (std::multimap<std::pair<double,int>, int,event_less>::iterator it = del.first; it != del.second; ++it)
						std::cout << "x: " << it->first.first << (it->first.second == SEG_END ? "end ":"start ") << " segment " << it->second << std::endl;
					std::cout << "Was looking at " << segments[rem_end_events[k]]->getEnd()->Coords()[2] << " " << segments[rem_end_events[k]]->getEnd()->GetHandle() << std::endl;
					std::cout << "Other side at " << segments[rem_end_events[k]]->getBeg()->Coords()[2] << " " << segments[rem_end_events[k]]->getBeg()->GetHandle() << std::endl;
					std::cout << "All events: " << events.size() << std::endl;
					for (std::multimap<std::pair<double, int>, int,event_less>::iterator it = events.begin(); it != events.end(); ++it)
						std::cout << "x: " << it->first.first << " type " << (it->first.second == SEG_START ? "start" : "end") << " segment " << it->second << " " << segments[it->second]->GetHandle() << " " << segments[it->second]->getBeg()->GetHandle() << " " << segments[it->second]->getEnd()->GetHandle() <<std::endl;
					
				}
				assert(flag);
			}
		}
		//storage for data
		std::vector< std::vector<char> > copy(transfer.size()*2);
		//memorize data
		{
			const int s[2] = {a,b};
			for(int q = 0; q < 2; ++q) //segments
				for(int k = 0; k < transfer.size(); ++k) //tags
				{
					int size = segments[s[q]].GetDataSize(transfer[k]);
					copy[k + q*transfer.size()].resize(transfer[k].GetBytesSize()*size);
					if( !copy.empty() ) segments[s[q]].GetData(transfer[k],0,size,&copy[k + q*transfer.size()][0]);
				}
		}
		ElementArray<Edge> splitted_a = Edge::SplitEdge(segments[a],ElementArray<Node>(m,1,I->GetHandle()),0);
		ElementArray<Edge> splitted_b = Edge::SplitEdge(segments[b],ElementArray<Node>(m,1,I->GetHandle()),0);
		//duplicate data
		{
			const Edge splitted[2][2] =
			{
				{splitted_a[0],splitted_a[1]},
				{splitted_b[0],splitted_b[1]},
			};
			for(int q = 0; q < 2;++q) //segments
				for(int k = 0; k < transfer.size();++k)
				{
					int size = (int)copy[k + q*transfer.size()].size() / transfer[k].GetBytesSize();
					if( size ) for(int l = 0; l < 2; ++l) //two parts
					{
						splitted[q][l].SetDataSize(transfer[k],size);
						splitted[q][l].SetData(transfer[k],0,size,&copy[k + q*transfer.size()][0]);
					}
				}
		}
		//replace segment a by new one
		segments[a] = splitted_a[0];
		//add event of ending of old segment
		if(print) std::cout << "1: Add segment " << a << " " << segments[a]->GetHandle() << " end" << " at " << I->Coords()[2] << " " << I->GetHandle() << std::endl;
		check_multimap(events,events.insert(std::make_pair(std::make_pair(I->Coords()[2],SEG_END), a)),checkmm);
		//put other side of segment
		segments.push_back(splitted_a[1]);
		//detect proper starting event
		if( segments.back()->getBeg()->Coords()[2] > segments.back()->getEnd()->Coords()[2] )
			segments.back()->SwapEnds();
		//add event of starting of new segment
		if(print) std::cout << "2: Add segment " << segments.size() - 1 << " " << segments.back()->GetHandle() << " start at " << segments.back()->getBeg()->Coords()[2] << " " << segments.back()->getBeg()->GetHandle() << std::endl;
		check_multimap(events,events.insert(std::make_pair(std::make_pair(segments.back()->getBeg()->Coords()[2],SEG_START), (int)segments.size() - 1)),checkmm);
		//add event of ending of new segment
		if(print) std::cout << "3: Add segment " << segments.size() - 1 << " " << segments.back()->GetHandle() << " end at " << segments.back()->getEnd()->Coords()[2] << " " << segments.back()->getEnd()->GetHandle() << std::endl;
		check_multimap(events,events.insert(std::make_pair(std::make_pair(segments.back()->getEnd()->Coords()[2],SEG_END),(int)segments.size() - 1)),checkmm);
		//replace segment b by new one
		segments[b] = splitted_b[0];
		//add event of ending of old segment
		if(print) std::cout << "4: Add segment " << b << " " << segments[b]->GetHandle() << " end at " << I->Coords()[2] << " " << I->GetHandle() << std::endl;
		check_multimap(events,events.insert(std::make_pair(std::make_pair(I->Coords()[2],SEG_END), b)),checkmm);
		//put other side of segment
		segments.push_back(splitted_b[1]);
		//detect proper starting event
		if( segments.back()->getBeg()->Coords()[2] > segments.back()->getEnd()->Coords()[2] )
			segments.back()->SwapEnds();
		//add event of starting of new segment
		if(print) std::cout << "5: Add segment " << segments.size() - 1 << " " << segments.back()->GetHandle() << " start at " << segments.back()->getBeg()->Coords()[2]<< " " << segments.back()->getBeg()->GetHandle() << std::endl;
		check_multimap(events,events.insert(std::make_pair(std::make_pair(segments.back()->getBeg()->Coords()[2],SEG_START), (int)segments.size() - 1)),checkmm);
		//add event of ending of new segment
		if(print) std::cout << "6: Add segment " << segments.size() - 1 << " " << segments.back()->GetHandle() << " end at " << segments.back()->getEnd()->Coords()[2] << " " << segments.back()->getEnd()->GetHandle() << std::endl;
		check_multimap(events,events.insert(std::make_pair(std::make_pair(segments.back()->getEnd()->Coords()[2],SEG_END), (int)segments.size() - 1)),checkmm);
		if (print)
		{
			std::cout << "Number of events: " << events.size() << std::endl;
			for (std::multimap<std::pair<double, int>, int,event_less>::iterator it = events.begin(); it != events.end(); ++it)
				std::cout << "x: " << it->first.first << " type " << (it->first.second == SEG_START ? "start" : "end") << " segment " << it->second << " " << segments[it->second]->GetHandle() << " " << segments[it->second]->getBeg()->GetHandle() << " " << segments[it->second]->getEnd()->GetHandle() << std::endl;
			assert(count_duplicates(segments) == 0);
		}
	}
	/*
	Point make_point(Edge e, int dir)
	{
		Storage::real_array ebeg = e->getBeg()->Coords();
		//Storage::real_array eend = e->getEnd()->Coords();
		//Storage::real v[3], vn[3], vc[3], vz[3] = {0,0,1};
		//make_vec(eend.data(),ebeg.data(),v);
		//cross_prod(vz,v,vn);
		//cross_prod(vz,vn,vc);
		//normalize(vc);
		return Point(ebeg[2],ebeg[dir]);
	}
	 */
	
	Point make_point(Edge e,  const Storage::real p0beg[3], const Storage::real  p0end[3], const Storage::real p1beg[3], const Storage::real p1end[3])
	{
		Storage::real_array ebeg = e->getBeg()->Coords();
		return project(ebeg.data(),p0beg,p0end,p1beg,p1end);
	}

	//intersect many segments
	void intersect(Mesh * m, ElementArray<Edge> & segments, ElementArray<Node> & nodes, std::vector<Tag> & transfer, const Storage::real p0beg[3], const Storage::real  p0end[3], const Storage::real p1beg[3], const Storage::real p1end[3], bool print)
	{
		std::map<position,Node,position_less> intersections;
		std::multimap<std::pair<double,int>,int,event_less> events;
		std::multimap<Point,int> sweep;
		double t1,t2;
		MarkerType initial = m->CreateMarker();
	
		if( print )
		{
			std::cout << "Input segments[" << segments.size() << "]: " << std::endl;
			for (ElementArray<Edge>::iterator it = segments.begin(); it != segments.end(); ++it)
				std::cout << "[ (" <<  it->getBeg()->Coords()[0] << "," << it->getBeg()->Coords()[1] << "," << it->getBeg()->Coords()[2] << "), ("<<  it->getEnd()->Coords()[0]   << "," << it->getEnd()->Coords()[1] << "," << it->getEnd()->Coords()[2] << ") ] " << std::endl;
			std::cout << "Create events based on segments." << std::endl;
		}

		for (int k = 0; k < (int)segments.size(); ++k)
		{
			if (segments[k]->getBeg()->Coords()[2] > segments[k]->getEnd()->Coords()[2])
				segments[k].SwapEnds();
			events.insert(std::make_pair(std::make_pair(segments[k]->getBeg()->Coords()[2],SEG_START),k));
			events.insert(std::make_pair(std::make_pair(segments[k]->getEnd()->Coords()[2],SEG_END), k));
			segments[k]->getBeg()->SetMarker(initial);
			segments[k]->getEnd()->SetMarker(initial);
			intersections.insert(std::make_pair(position(segments[k]->getBeg()->Coords().data()),segments[k]->getBeg()));
			intersections.insert(std::make_pair(position(segments[k]->getEnd()->Coords().data()),segments[k]->getEnd()));
		}


		if (print)
		{
			std::cout << "Number of events: " << events.size() << std::endl;
			for (std::multimap<std::pair<double, int>, int,event_less>::iterator it = events.begin(); it != events.end(); ++it)
				std::cout << "x: " << it->first.first << " type " << (it->first.second == SEG_START ? "start" : "end") << " segment " << it->second << " " << segments[it->second]->GetHandle() << " " << segments[it->second]->getBeg()->GetHandle() << " " << segments[it->second]->getEnd()->GetHandle() << std::endl;
		
			std::cout << " Start parsing events" << std::endl;
		}
	
		while (!events.empty())
		{
			std::multimap<std::pair<double,int>,int,event_less>::iterator first = events.begin();
			int t = first->first.second;
			int s = first->second;
			events.erase(first);
			if (t == SEG_START)
			{
				if( print ) std::cout << "Segment " << s << " start" << std::endl;
				//check if there is a line with same position
				Point p = make_point(segments[s],p0beg,p0end,p1beg,p1end);
				std::multimap<Point, int>::iterator ins = sweep.insert(std::make_pair(p, s));
				if (print)
				{
					std::cout << "Inserted into sweep" << std::endl;
					for (std::multimap<Point, int>::iterator it = sweep.begin(); it != sweep.end(); ++it)
						std::cout << "(" << it->first.x << "," << it->first.y << ")" << " segment " << it->second << std::endl;
				}
				//check line (or lines above current)
				for (int dir = 0; dir <= 1; ++dir) // look up or down
				{
					if( print ) std::cout << "Looking " << (dir ? "up" : "down") << std::endl;
					std::multimap<Point, int>::iterator iter = ins;
					while ((dir ? ++iter != sweep.end() : iter != sweep.begin())) //y is greater for next
					{
						if( !dir ) --iter;
						if (print) std::cout << "test " << s << " with " << iter->second << std::endl;
						if (segments[s]->getBeg() != segments[iter->second]->getBeg()) //ignore same starting position
						{
							if (print) std::cout << "checking intersection" << std::endl;
							std::pair<bool,Node> I = intersect_segments(m,segments[s], segments[iter->second],intersections,t1,t2,p0beg,p0end,p1beg,p1end,print);
							if (I.first)
							{
								if( print ) std::cout << "Intersection of " << s << " " <<segments[s]->GetHandle() << " " << segments[s]->getBeg()->GetHandle() << " " << segments[s]->getEnd()->GetHandle() << " and " << iter->second << " " << segments[iter->second]->GetHandle() << " " << segments[iter->second]->getBeg()->GetHandle() << " " << segments[iter->second]->getEnd()->GetHandle() << " at (" << I.second.Coords()[0] << "," << I.second.Coords()[1] << "," << I.second.Coords()[2] << ") t1 " << t1 << " t2 " << t2 << std::endl;
								intersect_event(m,s, iter->second, I.second, segments, sweep, events,transfer,print);
								//break;
							}
						}
						else if (print) std::cout << "skipping segments with same starting point" << std::endl;
						if ((2*dir-1)*(iter->first.y - ins->first.y) > 0) //visited line is above (below) current
							break; //stop search
					}
				}
			}
			else if (t == SEG_END)
			{
				if( print ) std::cout << "Segment " << s << " end" << std::endl;
				//remove segment from sweep
				Point p = make_point(segments[s],p0beg,p0end,p1beg,p1end);
				std::pair< std::multimap<Point, int>::iterator, std::multimap<Point, int>::iterator > range = sweep.equal_range(p);
				if( print ) std::cout << "Range distance " << std::distance(range.first,range.second) << " sweep size " << sweep.size() << std::endl;
				std::multimap<Point, int>::iterator above = range.second, below = range.first;
				bool flag = false, test = true;
				if( below == sweep.begin() ) test = false;
				else --below;
				if( above == sweep.end() ) test = false;
				if( test && print ) std::cout << "Test will be performed" << std::endl;
				for (std::multimap<Point, int>::iterator it = range.first; it != range.second; ++it) //search over all events
				{
					if( it->second == s) //found necessery segment
					{
						if (print)
						{
							std::cout << "Erase segment " << s << " from sweep: " << std::endl;
							for (std::multimap<Point, int>::iterator it = sweep.begin(); it != sweep.end(); ++it)
								std::cout << "(" << it->first.x << "," << it->first.y << ")" << " segment " << it->second << std::endl;
						}
						sweep.erase(it);
						flag = true;
						break; //do not expect any more
					}
				}
				if (!flag) std::cout << __FILE__ << ":" << __LINE__ <<  " Error: cannot find segment " << s << " in sweep" << std::endl;
				assert(flag);
				if (test)
				{
					if (print) std::cout << "test " << below->second << " with " << above->second << std::endl;
					if (segments[above->second]->getBeg() != segments[below->second]->getBeg())
					{
						if (print) std::cout << "checking intersection" << std::endl;
						std::pair<bool,Node> I = intersect_segments(m, segments[below->second], segments[above->second],intersections,t1,t2,p0beg,p0end,p1beg,p1end,print);
						if (I.first)
						{
							if( print ) std::cout << "Intersection of " << below->second << " " << segments[below->second]->GetHandle() << " " << segments[below->second]->getBeg()->GetHandle() << " " << segments[below->second]->getEnd()->GetHandle() << " and " << above->second << " " << segments[above->second]->GetHandle() << " " << segments[above->second]->getBeg()->GetHandle() << " " << segments[above->second]->getEnd()->GetHandle() << " at (" << I.second.Coords()[0] << "," << I.second.Coords()[1] << "," << I.second.Coords()[2] << ") t1 " << t1 << " t2 " << t2 << std::endl;
							intersect_event(m,below->second, above->second, I.second, segments, sweep, events,transfer,print);
						}
					}
					else if (print) std::cout << "skipping segments with same starting point" << std::endl;
				}
			}
		}
		//copy intersections
		nodes.clear();
		for(std::map<position,Node,position_less>::iterator it = intersections.begin(); it != intersections.end(); ++it)
		{
			if( !it->second->GetMarker(initial) )
				nodes.push_back(it->second);
			else it->second->RemMarker(initial);
		}
		m->ReleaseMarker(initial);
	}
	
	void block_number_intersection(ElementArray<Element> & adj, Tag block, std::vector<int> & out)
	{
		Storage::integer_array be = adj[0]->IntegerArray(block);
		std::vector<int> inter(be.begin(),be.end()), tmp(inter.size());
		for(ElementArray<Edge>::size_type k = 1; k < adj.size(); ++k)
		{
			be = adj[k]->IntegerArray(block);
			tmp.resize(std::set_intersection(inter.begin(),inter.end(),be.begin(),be.end(),tmp.begin())-tmp.begin());
			inter.swap(tmp);
		}
		out.clear();
		out.insert(out.end(),inter.begin(),inter.end());
		//Storage::integer_array bn = n->IntegerArray(block);
		//bn.replace(bn.begin(),bn.end(),inter.begin(),inter.end());
	}

	void block_number_union(Element n, ElementArray<Element> & adj, Tag block)
	{
		Storage::integer_array be = adj[0]->IntegerArray(block);
		std::vector<int> uni(be.begin(),be.end()), tmp(uni.size());
		for(ElementArray<Edge>::size_type k = 1; k < adj.size(); ++k)
		{
			be = adj[k]->IntegerArray(block);
			tmp.resize(uni.size()+be.size());
			tmp.resize(std::set_union(uni.begin(),uni.end(),be.begin(),be.end(),tmp.begin())-tmp.begin());
			uni.swap(tmp);
		}
		Storage::integer_array bn = n->IntegerArray(block);
		bn.replace(bn.begin(),bn.end(),uni.begin(),uni.end());
	}
	

	void Mesh::LoadECL(std::string File)
	{
		std::cout << std::scientific;
		FILE * f = fopen(File.c_str(),"r");
		if( f == NULL )
		{
			std::cout << __FILE__ << ":" << __LINE__ << " cannot open file " << File << std::endl;
			throw BadFileName;
		}
		std::vector<HandleType> old_nodes(NumberOfNodes());
		{
			unsigned qq = 0;
			for(Mesh::iteratorNode it = BeginNode(); it != EndNode(); ++it)
				old_nodes[qq++] = *it;
		}
		if( !old_nodes.empty() ) 
			std::sort(old_nodes.begin(),old_nodes.end(),CentroidComparator(this));
		std::vector< std::pair< std::pair<FILE *,std::string>, int> > fs(1,std::make_pair(std::make_pair(f,File),0));
		char readline[2048], *p, *pend, rec[2048];
		int text_end, text_start, state = ECL_NONE, nchars;
		int waitlines = 0;
		int have_dimens = 0, totread, downread, numrecs, offset;
		int gtype = ECL_GTYPE_NONE;
		int argtype = ECL_VAR_NONE;
		int radial = ECL_GTYPE_NONE;
		Storage::real * read_arrayf = NULL;
		Storage::integer * read_arrayi = NULL;
		Storage::integer dims[3], mapaxis[6] = {0,1,0,0,1,0};
		Storage::real inrad = 0;
		std::vector<Storage::real> xyz,perm,poro, tops,zcorn;
		std::vector<Storage::integer> actnum;
		while(!fs.empty())
		{
			while(fgets(readline,2048,fs.back().first.first) != NULL)
			{
				fs.back().second++; //line number
				{
					if( readline[strlen(readline)-1] == '\n' ) readline[strlen(readline)-1] = '\0';
					text_end = static_cast<int>(strlen(readline));
					for(text_start = 0; isspace(readline[text_start]) && text_start < text_end; text_start++);
					if( text_start == text_end ) continue;
					for(text_end = text_end-1; isspace(readline[text_end]) && text_end > text_start; text_end--);
					readline[text_end+1] = '\0';
					p = readline + text_start;
					pend = readline + text_end + 1;
					for(char * q = p; q < pend; q++) *q = toupper(*q);
				}
				if( p[0] == '-' && p[1] == '-' ) continue; //skip comment
				if(waitlines) {waitlines--; continue;} //skip meaningful lines
				switch(state)
				{
				case ECL_NONE:
					if( !ECLSTRCMP(p,"END") ) //end of data - don't read beyond
					{
						goto ecl_exit_loop;
					}
					else if( !ECLSTRCMP(p,"INCLUDE") ) state = ECL_INCLUDE;
					else if( !ECLSTRCMP(p,"DIMENS") || !ECLSTRCMP(p,"SPECGRID") )
					{
						read_arrayi = dims;
						numrecs = 1;
						downread = totread = 3;
						argtype = ECL_VAR_INT;
						offset = state = ECL_DIMENS;
						have_dimens = 1;
					}
					else if( !ECLSTRCMP(p,"MAPAXIS") )
					{
						read_arrayi = mapaxis;
						numrecs = 1;
						downread = totread = 6;
						argtype = ECL_VAR_INT;
						offset = state = ECL_MAPAXIS;
					}
					else if( !ECLSTRCMP(p,"DX") )
					{
						assert(have_dimens);
						if( xyz.empty() ) xyz.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = xyz.empty()? NULL : &xyz[0];
						numrecs = 3;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = state = ECL_DX;
					}
					else if( !ECLSTRCMP(p,"DY") )
					{
						assert(have_dimens);
						if( xyz.empty() ) xyz.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = xyz.empty() ? NULL : &xyz[0];
						numrecs = 3;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = ECL_DX;
						state = ECL_DY;
					}
					else if( !ECLSTRCMP(p,"DZ") )
					{
						assert(have_dimens);
						if( xyz.empty() ) xyz.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = xyz.empty() ? NULL : &xyz[0];
						numrecs = 3;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = ECL_DX;
						state = ECL_DZ;
					}
					else if( !ECLSTRCMP(p,"COORD") )
					{
						assert(have_dimens);
						if( xyz.empty() ) xyz.resize(3*2*(dims[0]+1)*(dims[1]+1));
						read_arrayf = xyz.empty() ? NULL : &xyz[0];
						numrecs = 1;
						downread = totread = 3*2*(dims[0]+1)*(dims[1]+1);
						argtype = ECL_VAR_REAL;
						offset = state = ECL_COORDS;
						gtype = ECL_GTYPE_ZCORN;
					}
					else if( !ECLSTRCMP(p,"ZCORN") )
					{
						assert(have_dimens);
						if( zcorn.empty() ) zcorn.resize(dims[0]*dims[1]*dims[2]*8);
						read_arrayf = zcorn.empty() ? NULL : &zcorn[0];
						numrecs = 1;
						downread = totread = dims[0]*dims[1]*dims[2]*8;
						argtype = ECL_VAR_REAL;
						state = offset = ECL_ZCORN;
					}
					else if( !ECLSTRCMP(p,"TOPS") )
					{
						assert(have_dimens);
						tops.resize(dims[0]*dims[1]*dims[2]);
						read_arrayf = tops.empty() ? NULL : &tops[0];
						numrecs = 1;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = state = ECL_TOPS;
						gtype = ECL_GTYPE_TOPS;
					}
					else if( !ECLSTRCMP(p,"PERMX") )
					{
						assert(have_dimens);
						if( perm.empty() ) perm.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = perm.empty() ? NULL : &perm[0];
						numrecs = 3;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = state = ECL_PERMX;
					}
					else if( !ECLSTRCMP(p,"PERMY") )
					{
						assert(have_dimens);
						if( perm.empty() ) perm.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = perm.empty() ? NULL : &perm[0];
						numrecs = 3;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = ECL_PERMX;
						state = ECL_PERMY;
					}
					else if( !ECLSTRCMP(p,"PERMZ") )
					{
						assert(have_dimens);
						if( perm.empty() ) perm.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = perm.empty() ? NULL : &perm[0];
						numrecs = 3;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = ECL_PERMX;
						state = ECL_PERMZ;
					}
					else if( !ECLSTRCMP(p,"PORO") )
					{
						assert(have_dimens);
						poro.resize(3*dims[0]*dims[1]*dims[2]);
						read_arrayf = poro.empty() ? NULL : &poro[0];
						numrecs = 1;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_REAL;
						offset = state = ECL_PORO;
					}
					else if( !ECLSTRCMP(p,"ACTNUM") )
					{
						assert(have_dimens);
						actnum.resize(dims[0]*dims[1]*dims[2]);
						read_arrayi = actnum.empty() ? NULL : &actnum[0];
						numrecs = 1;
						downread = totread = dims[0]*dims[1]*dims[2];
						argtype = ECL_VAR_INT;
						offset = state = ECL_ACTNUM;
					}
					else if( !ECLSTRCMP(p,"RADIAL") )
					{
						radial = ECL_GTYPE_RADIAL;
					}
					else if( !ECLSTRCMP(p,"CART") )
					{
						radial = ECL_GTYPE_CARTESIAN;
					}
					else if( !ECLSTRCMP(p,"INRAD") )
					{
						if( radial != ECL_GTYPE_RADIAL ) 
						{
							std::cout << __FILE__ << ":" << __LINE__ << " inner radius specified for cartesian grid ";
							std::cout << " in " << fs.back().first.second << ":" << fs.back().second << std::endl;
						}
						if( radial == ECL_GTYPE_NONE ) radial = ECL_GTYPE_RADIAL;
						state = ECL_INRAD;
					}
					else
					{
						std::cout << __FILE__ << ":" << __LINE__ << " skipped " << p << " in " << fs.back().first.second << ":" << fs.back().second << std::endl;
					}
					break;
				case ECL_SKIP_SECTION:
					if( *(pend-1) == '/' || *p == '/' ) state = ECL_NONE;
					break;
				case ECL_INCLUDE:
					if( 1 == sscanf(p," %s",rec) )
					{
						int shift_one = 0;
						if( (rec[0] == '\'' || rec[0] == '"') && rec[0] == rec[strlen(rec)-1] ) //remove quotes
						{
							rec[strlen(rec)-1] = '\0';
							shift_one = 1;
						}
						f = fopen(rec+shift_one,"r");
						if( f == NULL )
						{
							std::cout << __FILE__ << ":" << __LINE__ << " cannot open file " << rec+shift_one << " included from "<< fs.back().first.second << " line " << fs.back().second << std::endl;
							throw BadFileName;
						}
						fs.push_back(std::make_pair(std::make_pair(f,std::string(rec+shift_one)),0));
						if( *(pend-1) == '/' ) state = ECL_NONE; else state = ECL_SKIP_SECTION;
					}
					else
					{
						std::cout << __FILE__ << ":" << __LINE__ << " cannot read file name, string " << p << " in " << fs.back().first.second << " line " << fs.back().second << std::endl;
						throw BadFile;
					}
					break;
				case ECL_ZCORN:
				case ECL_COORDS:
				case ECL_MAPAXIS:
				case ECL_DIMENS:
				case ECL_DX:
				case ECL_DY:
				case ECL_DZ:
				case ECL_PERMX:
				case ECL_PERMY:
				case ECL_PERMZ:
				case ECL_PORO:
				case ECL_ACTNUM:
				case ECL_TOPS:
					while( downread > 0 && p < pend )
					{
						if( 1 == sscanf(p,"%s%n",rec,&nchars) )
						{
							p += nchars;
							while(isspace(*p) && p < pend) ++p;
							int count = 1;
							int begval = 0;
							for(int q = 0; q < static_cast<int>(strlen(rec)); ++q)
								if( rec[q] == '*' )
								{
									begval = q+1;
									rec[q] = '\0';
									break;
								}
							if( begval > 0 ) count = atoi(rec);
							if( argtype == ECL_VAR_REAL )
							{
								Storage::real val = atof(rec+begval);
								while(count)
								{
									read_arrayf[numrecs*(totread-(downread--))+(state-offset)] = val;
									count--;
								}
							}
							else if( argtype == ECL_VAR_INT )
							{
								Storage::integer val = atoi(rec+begval);
								while(count)
								{
									read_arrayi[numrecs*(totread-(downread--))+(state-offset)] = val;
									count--;
								}
							}
							else
							{
								std::cout << __FILE__ << ":" << __LINE__ << " probably forgot to set up argument type to read " << std::endl;
								throw Impossible;
							}
						}
						else
						{
							std::cout << __FILE__ << ":" << __LINE__ << " cannot read data " << p << " in " << fs.back().first.second << ":" << fs.back().second << std::endl;
							throw BadFile;
						}
						if( *p == '/' ) 
						{
							if( downread > 0 )
							{
								std::cout << __FILE__ << ":" << __LINE__ << " early data termination, read " << totread-downread << " of " << totread << " records in " << fs.back().first.second << ":" << fs.back().second << std::endl;
								throw BadFile;
							}
						}
					}
					if( *(pend-1) == '/' || *p == '/' ) state = ECL_NONE; else if( downread == 0 ) state = ECL_SKIP_SECTION;
					break;
				case ECL_INRAD:
					if( 1 == sscanf(p,"%lf%n",&inrad,&nchars) )
					{
						p += nchars;
						while(isspace(*p) && p < pend) ++p;
					}
					else
					{
						std::cout << __FILE__ << ":" << __LINE__ << " cannot read data " << p << " in " << fs.back().first.second << ":" << fs.back().second << std::endl;
						throw BadFile;
					}
					if( *(pend-1) == '/' || *p == '/' ) state = ECL_NONE; else state = ECL_SKIP_SECTION;
					break;
				}
			}
ecl_exit_loop:
			fclose(fs.back().first.first);
			fs.pop_back();
		}
		if( radial == ECL_GTYPE_RADIAL )
		{
			std::cout << __FILE__ << ":" << __LINE__ << " radial grids not supported yet " << std::endl;
		}
		if( gtype == ECL_GTYPE_TOPS )
		{
			
			std::vector<HandleType> newnodes((dims[0]+1)*(dims[1]+1)*(dims[2]+1));
			Storage::real x, y, z, node_xyz[3];
			x = 0.0;
			int numnode = 0;
			for(int i = 0; i < dims[0]+1; i++)
			{
				Storage::integer pif = std::min(dims[0]-1,i), pib = std::max(i-1,0);
				y = 0.0;
				for(int j = 0; j < dims[1]+1; j++)
				{
					Storage::integer pjf = std::min(dims[1]-1,j), pjb = std::max(j-1,0);
					z = (
							tops[ECL_IJK_DATA(pib,pjb,0)]+
							tops[ECL_IJK_DATA(pib,pjf,0)]+
							tops[ECL_IJK_DATA(pif,pjb,0)]+
							tops[ECL_IJK_DATA(pif,pjf,0)]
						  )*0.25;
					z -= (
							xyz[3*ECL_IJK_DATA(pib,pjb,0)+2]+
							xyz[3*ECL_IJK_DATA(pib,pjf,0)+2]+
							xyz[3*ECL_IJK_DATA(pif,pjb,0)+2]+
							xyz[3*ECL_IJK_DATA(pif,pjf,0)+2]
							)*0.25;
					for(int k = 0; k < dims[2]+1; k++)
					{
						Storage::integer pkf = std::min(dims[2]-1,k), pkb = std::max(k-1,0);
						node_xyz[0] = x;
						node_xyz[1] = y;
						node_xyz[2] = z;
						int find = -1;
						if( !old_nodes.empty() )
						{
							std::vector<HandleType>::iterator it = std::lower_bound(old_nodes.begin(),old_nodes.end(),node_xyz,CentroidComparator(this));
							if( it != old_nodes.end() ) 
							{
								Storage::real_array c = RealArrayDF(*it,CoordsTag());
								if( CentroidComparator(this).Compare(node_xyz,c.data()) == 0 )
									find = static_cast<int>(it - old_nodes.begin());
							}
						}
						if( find == -1 ) newnodes[numnode++] = CreateNode(node_xyz)->GetHandle();
						else newnodes[numnode++] = old_nodes[find];
						//std::cout << i << " " << j << " " << k << " ( " << x << " , " << y << " , " << z << ") " << newnodes.back()->LocalID() << std::endl; 
						x += (
								(
								  xyz[3*ECL_IJK_DATA(pib,pjb,pkf)+0]+
								  xyz[3*ECL_IJK_DATA(pib,pjf,pkf)+0]+
								  xyz[3*ECL_IJK_DATA(pif,pjb,pkf)+0]+
								  xyz[3*ECL_IJK_DATA(pif,pjf,pkf)+0]
								)
							-
								(
								  xyz[3*ECL_IJK_DATA(pib,pjb,pkb)+0]+
								  xyz[3*ECL_IJK_DATA(pib,pjf,pkb)+0]+
								  xyz[3*ECL_IJK_DATA(pif,pjb,pkb)+0]+
								  xyz[3*ECL_IJK_DATA(pif,pjf,pkb)+0]
								)
								)*0.25;
						y += (
								(
							      xyz[3*ECL_IJK_DATA(pib,pjb,pkf)+1]+
								  xyz[3*ECL_IJK_DATA(pib,pjf,pkf)+1]+
								  xyz[3*ECL_IJK_DATA(pif,pjb,pkf)+1]+
								  xyz[3*ECL_IJK_DATA(pif,pjf,pkf)+1]
								)
							-
								(
								  xyz[3*ECL_IJK_DATA(pib,pjb,pkb)+1]+
								  xyz[3*ECL_IJK_DATA(pib,pjf,pkb)+1]+
								  xyz[3*ECL_IJK_DATA(pif,pjb,pkb)+1]+
								  xyz[3*ECL_IJK_DATA(pif,pjf,pkb)+1]
								)
								)*0.25;
						z += (
								xyz[3*ECL_IJK_DATA(pib,pjb,pkb)+2]+
								xyz[3*ECL_IJK_DATA(pib,pjf,pkb)+2]+
								xyz[3*ECL_IJK_DATA(pif,pjb,pkb)+2]+
								xyz[3*ECL_IJK_DATA(pif,pjf,pkb)+2]+
							    xyz[3*ECL_IJK_DATA(pib,pjb,pkf)+2]+
								xyz[3*ECL_IJK_DATA(pib,pjf,pkf)+2]+
								xyz[3*ECL_IJK_DATA(pif,pjb,pkf)+2]+
								xyz[3*ECL_IJK_DATA(pif,pjf,pkf)+2]
								)*0.125;
					}
					y += (
							xyz[3*ECL_IJK_DATA(pib,pjb,0)+1]+
							xyz[3*ECL_IJK_DATA(pif,pjb,0)+1]+
							xyz[3*ECL_IJK_DATA(pib,pjf,0)+1]+
							xyz[3*ECL_IJK_DATA(pif,pjf,0)+1]
							)*0.25; 
				}
				x += (
						xyz[3*ECL_IJK_DATA(pib,0,0)+0]+
						xyz[3*ECL_IJK_DATA(pif,0,0)+0]
						)*0.5; 
			}
			Tag tagporo,tagperm;
			if( !poro.empty() ) tagporo = CreateTag("PORO",DATA_REAL,CELL,NONE,1);
			if( !perm.empty() ) tagperm = CreateTag("PERM",DATA_REAL,CELL,NONE,3);

			const Storage::integer nvf[24] = { 2, 3, 1, 0, 4, 5, 7, 6, 0, 1, 5, 4, 3, 2, 6, 7, 2, 0, 4, 6, 1, 3, 7, 5 };
			const Storage::integer numnodes[6] = { 4, 4, 4, 4, 4, 4 };
			for(int i = 0; i < dims[0]; i++)
				for(int j = 0; j < dims[1]; j++)
					for(int k = 0; k < dims[2]; k++)
					{
						HandleType verts[8];
						verts[0] = newnodes[((i+0)*(dims[1]+1)+(j+0))*(dims[2]+1)+(k+0)];
						verts[1] = newnodes[((i+1)*(dims[1]+1)+(j+0))*(dims[2]+1)+(k+0)];
						verts[2] = newnodes[((i+0)*(dims[1]+1)+(j+1))*(dims[2]+1)+(k+0)];
						verts[3] = newnodes[((i+1)*(dims[1]+1)+(j+1))*(dims[2]+1)+(k+0)];
						verts[4] = newnodes[((i+0)*(dims[1]+1)+(j+0))*(dims[2]+1)+(k+1)];
						verts[5] = newnodes[((i+1)*(dims[1]+1)+(j+0))*(dims[2]+1)+(k+1)];
						verts[6] = newnodes[((i+0)*(dims[1]+1)+(j+1))*(dims[2]+1)+(k+1)];
						verts[7] = newnodes[((i+1)*(dims[1]+1)+(j+1))*(dims[2]+1)+(k+1)];
						//for(int q = 0; q < 8; q++)
						//	std::cout << verts[q]->Coords()[0] << " " << verts[q]->Coords()[1] << " " << verts[q]->Coords()[2] << " " << verts[q]->LocalID() << std::endl;
						Cell c = CreateCell(ElementArray<Node>(this,verts,verts+8),nvf,numnodes,6).first;
						if( !poro.empty() ) c->RealDF(tagporo) = poro[(i*dims[1]+j)*dims[2]+k];
						if( !perm.empty() )
						{
							Storage::real_array arr_perm = c->RealArrayDF(tagperm);
							arr_perm[0] = perm[3*((i*dims[1]+j)*dims[2]+k)+0];
							arr_perm[1] = perm[3*((i*dims[1]+j)*dims[2]+k)+1];
							arr_perm[2] = perm[3*((i*dims[1]+j)*dims[2]+k)+2];
						}
					}
		}
		else if( gtype == ECL_GTYPE_ZCORN )
		{
			SetTopologyCheck(PRINT_NOTIFY | NEED_TEST_CLOSURE | PROHIBIT_MULTIPOLYGON | PROHIBIT_MULTILINE | MARK_ON_ERROR);
			SetTopologyCheck(DEGENERATE_EDGE | DEGENERATE_FACE | DEGENERATE_CELL);
			SetTopologyCheck(TRIPLE_SHARED_FACE | FLATTENED_CELL | INTERLEAVED_FACES);
			SetTopologyCheck(DUPLICATE_EDGE | DUPLICATE_FACE | DUPLICATE_CELL);
			SetTopologyCheck(ADJACENT_DUPLICATE | ADJACENT_DIMENSION);
			RemTopologyCheck(THROW_EXCEPTION);

			//make arrays more human-readable
			std::vector< std::vector< std::vector< std::vector< Storage::real > > > > zcorn_array, coords_array;
			{
				zcorn_array.resize(dims[0]);
				for(int i = 0; i < dims[0]; i++)
				{
					zcorn_array[i].resize(dims[1]);
					for(int j = 0; j < dims[1]; j++)
					{
						zcorn_array[i][j].resize(dims[2]);
						for(int k = 0; k < dims[2]; k++)
							zcorn_array[i][j][k].resize(8);
					}
				}
				int pos = 0;
				for(int k = 0; k < dims[2]; k++)
				{
					for(int q = 0; q < 2; q++)
						for(int j = 0; j < dims[1]; j++)
							for(int m = 0; m < 2; m++)
								for(int i = 0; i < dims[0]; i++)
									for(int l = 0; l < 2; l++)
										zcorn_array[i][j][k][l+m*2+(1-q)*4] = zcorn[pos++];
				}
			}
			{
				coords_array.resize(dims[0]+1);
				for(int i = 0; i < dims[0]+1; i++)
				{
					coords_array[i].resize(dims[1]+1);
					for(int j = 0; j < dims[1]+1; j++)
					{
						coords_array[i][j].resize(2);
						for(int l = 0; l < 2; l++)
							coords_array[i][j][l].resize(3);
					}
				}
				int pos = 0;
				for(int j = 0; j < dims[1]+1; j++)
					for(int i = 0; i < dims[0]+1; i++)
						for(int l = 0; l < 2; l++)
							for(int k = 0; k < 3; k++)
								coords_array[i][j][l][k] = xyz[pos++];
			}
			//assemble pillars
			{
				//Tag node_number = CreateTag("NODE_NUMBER",DATA_INTEGER,NODE,NONE);
				Tag cell_number = CreateTag("CELL_NUMBER",DATA_INTEGER,CELL,NONE,1);
				Tag edge_number = CreateTag("EDGE_NUMBER",DATA_INTEGER,EDGE,NONE);
				Tag block_number = CreateTag("BLOCK_NUMBER",DATA_INTEGER,EDGE|NODE,NONE);
				typedef std::map<Storage::real,Node> pillar;
				std::vector< pillar > pillars((dims[0]+1)*(dims[1]+1));
				for(int i = 0; i < dims[0]; i++)
				{
					for(int j = 0; j < dims[1]; j++)
					{
						for(int k = 0; k < dims[2]; k++)
						{
							for(int l = 0; l < 8; ++l)
							{
								//pillar indexes
								int i2 = i + l%2, j2 = j + (l/2)%2;
								pillar & p = pillars[i2*(dims[1]+1) + j2];
								pillar::iterator search = p.find(zcorn_array[i][j][k][l]);
								Node n; //node added to pillar
								if( search == p.end() )
								{
									Storage::real node_xyz[3];
									Storage::real alpha = (zcorn_array[i][j][k][l]-coords_array[i2][j2][1][2])/(coords_array[i2][j2][0][2]-coords_array[i2][j2][1][2]);
									node_xyz[0] = coords_array[i2][j2][1][0] + alpha * (coords_array[i2][j2][0][0] - coords_array[i2][j2][1][0]);
									node_xyz[1] = coords_array[i2][j2][1][1] + alpha * (coords_array[i2][j2][0][1] - coords_array[i2][j2][1][1]);
									node_xyz[2] = zcorn_array[i][j][k][l];
									int find = -1;
									if( !old_nodes.empty() )
									{
										std::vector<HandleType>::iterator it = std::lower_bound(old_nodes.begin(),old_nodes.end(),node_xyz,CentroidComparator(this));
										if( it != old_nodes.end() )
										{
											Storage::real_array c = RealArrayDF(*it,CoordsTag());
											if( CentroidComparator(this).Compare(node_xyz,c.data()) == 0 )
												find = static_cast<int>(it - old_nodes.begin());
										}
									}
									n = find == -1 ? CreateNode(node_xyz) : Node(this,old_nodes[find]);
									p.insert(std::make_pair(zcorn_array[i][j][k][l],n));
								} else n = search->second; //if
								//n->IntegerArray(node_number).push_back(l);
								//n->IntegerArray(block_number).push_back((i + (j+k*dims[1])*dims[0]));
							} //l
						} //k
					} //j
				} //i
				/*      (6)*<<------[3]--------*(7)
						  /|                  /|
						[6]                 [7]|
						/  |                /  |
					(4)*--------[2]------>>*(5)|
					   |  [10]             |  [11]
					   |                   |   |
					   |   |               |   |
					   |                   |   |
					  [8]  |              [9]  |
					   |(2)*- - - - [1]- - |->>*(3)
					   |  /                |  /
					   |[4]                |[5]
					   |/                  |/
					(0)*<<------[0]--------*(1)
				*/
				//structure to create an edge from pair of nodes
				ElementArray<Node> edge_nodes(this,2);
				//all edges along pillars
				std::vector< ElementArray<Edge> > pillar_edges((dims[0]+1)*(dims[1]+1),ElementArray<Edge>(this));
				//all edges of each block
				//std::vector< std::vector<Edge> > block_edges(dims[0]*dims[1]*dims[2]);
				//create edges along pillars
				for(int i = 0; i < dims[0]+1; ++i)
				{
					for(int j = 0; j < dims[1]+1; ++j)
					{
						pillar & p = pillars[i*(dims[1]+1)+j];
						ElementArray<Edge> & p_edges = pillar_edges[i*(dims[1]+1)+j];
						pillar::iterator it = p.begin();
						pillar::iterator pre_it = it++;
						while(it != p.end())
						{
							edge_nodes[0] = it->second;
							edge_nodes[1] = pre_it->second;
							p_edges.push_back(CreateEdge(edge_nodes).first);
							pre_it = it++;
						}
					} //j
				} //i
				//mark edges along pillars (vertical edges, 8, 9, 10, 11)
				for(int i = 0; i < dims[0]; ++i)
				{
					for(int j = 0; j < dims[1]; ++j)
					{
						for(int k = 0; k < dims[2]; ++k)
						{
							const int edge_nodes[4][2] = 
							{
								{0,4}, //edge 8
								{1,5}, //edge 9
								{2,6}, //edge 10
								{3,7}  //edge 11
							}; //nodes of edges 8,9,10,11, bottom->top
							for(int l = 0; l < 4; ++l) //loop edges
							{
								HandleType find[2];
								int i2 = i + l%2, j2 = j + l/2;
								pillar & p = pillars[i2*(dims[1]+1) + j2];
								pillar::iterator b,t, it, jt; 
								//find nodes in pillar that belong to current block
								std::vector<double> & debug = zcorn_array[i][j][k];
								double pa = zcorn_array[i][j][k][edge_nodes[l][0]];
								double pb = zcorn_array[i][j][k][edge_nodes[l][1]];
								if(pa > pb) //otherwise run iterator in different direction
								{
									b = p.find(pb); //bottom
									t = p.find(pa); //top
								}
								else
								{
									b = p.find(pa); //bottom
									t = p.find(pb); //top
								}
								//go over edges
								it = b;
								while(it != t)
								{
									jt = it++; // jt<->it forms a segment
									find[0] = jt->second->GetHandle();
									find[1] = it->second->GetHandle();
									Edge e(this,FindSharedAdjacency(find,2)); //find edge
									e->IntegerArray(block_number).push_back((i + (j+k*dims[1])*dims[0]));
									e->IntegerArray(edge_number).push_back(8+l);
									//block_edges[(i + (j+k*dims[1])*dims[0])].push_back(e);
								}
							}
						}
					}
				}
				
				Tag pillar_mark = CreateTag("PILLAR_MARK",DATA_INTEGER,EDGE,NONE,3);
				//set of lines to be intersected
				ElementArray<Edge> edges(this);
				ElementArray<Node> intersections(this);
				//tags to be transfered
				std::vector<Tag> transfer(3);
				transfer[0] = edge_number;
				transfer[1] = block_number;
				transfer[2] = pillar_mark;
				//for sorting a pair of data
				std::vector<int> indices_sort, temporary;
				//array to obtain block number
				std::vector<int> block_number_inter;
				//store faces for each block to assemble block cells later
				std::vector< ElementArray<Face> > block_faces(dims[0]*dims[1]*dims[2],ElementArray<Face>(this));
				//store top-bottom edges to assemble top and bottom faces later
				std::vector< ElementArray<Edge> > block_edges(dims[0]*dims[1]*dims[2],ElementArray<Edge>(this));
				//store edges along pillar to assemble faces along pillars, on back side of the pillar
				std::vector< ElementArray<Edge> > pillar_block_edges_back(dims[2],ElementArray<Edge>(this));
				//on front side of the pillar
				std::vector< ElementArray<Edge> > pillar_block_edges_front(dims[2],ElementArray<Edge>(this));
				//mark used nodes to detect top pillar node, bottom pillar node and pillar edge dropouts
				MarkerType used = CreateMarker();
				//mark edges on pillar for adjacency retrival
				MarkerType mrk = CreateMarker();
				//mark original edges of each block face, so that we know outer boundary on constructed interface
				MarkerType outer = CreateMarker();
				//print out intersection algorithm
				bool print_inter = false;
				//print out block number information
				bool print_bn = false;
				//print out block edges information
				bool print_bedges = false;
				//some intermediate info
				bool print_info = false;
				

				//go over nx pairs of pillars, then ny pairs of pillars
				for(int q = 0; q < 2; ++q)
				{
					for(int i = 0; i < dims[0]+q; i++)
					{
						printf("%s %6.2f%%\r",q ? "ny":"nx", ((double)i)/((double)dims[0]+q-1)*100);
						fflush(stdout);
						for(int j = 0; j < dims[1]+!q; ++j)
						{
							if( print_info )
							{
								std::cout << "working on " << (q ? "ny" : "nx") << " pair of pillars: " << i << "," << j << " and " << i+!q << "," << j+q << std::endl;
							}
							//p0 is at i,j
							//p1 is at i+1,j, when q = 0 and at i,j+1, when q = 1
							pillar & p0 = pillars[(i+ 0)*(dims[1]+1)+j+0];
							pillar & p1 = pillars[(i+!q)*(dims[1]+1)+j+q];
							
							for(pillar::iterator it = p0.begin(); it != p0.end(); ++it)  it->second.RemMarker(used);
							for(pillar::iterator it = p1.begin(); it != p1.end(); ++it)  it->second.RemMarker(used);
							
							//preallocate array for edges
							edges.reserve(2*std::max(p0.size(),p1.size()));
							//remember visited edges
							MarkerType visited = CreateMarker();
							//add far faces of j-1 block
							if( (1-q)*j + q*i > 0 ) //test j when q = 0 and i when q = 1
							{
								if( print_info ) std::cout << "back cell: " << i-q << "," << j-!q << std::endl;
								for(int k = 0; k < dims[2]; ++k)
								{
									for(int l = 0; l < 2; ++l) //top-bottom
									{
										//q = 0 - test 2,3 + 4*l (edge 1 and 3)
										//q = 1 - test 1,3 + 4*l (edge 5 and 7)
										edge_nodes[0] = p0[zcorn_array[i-q][j-!q][k][2 - q + 4*l]];
										edge_nodes[1] = p1[zcorn_array[i-q][j-!q][k][3 - 0 + 4*l]];
										if( edge_nodes[0] != edge_nodes[1] )
										{
											edge_nodes.SetMarker(used);
											Edge e = CreateEdge(edge_nodes).first;
											if( !e->GetMarker(visited) )
											{
												edges.push_back(e);
												e->SetMarker(visited);
											}
											e->IntegerArray(block_number).push_back((i-q + (j-!q+k*dims[1])*dims[0]));
											e->IntegerArray(edge_number).push_back(1 + 2*l + 4*q);
											Storage::integer_array mark = e->IntegerArray(pillar_mark);
											//back block indices
											mark[0] = i;
											mark[1] = j;
											//nx or ny
											mark[2] = q;
										}
									}
								}
							}
							//add near faces of j block
							if( (1-q)*j + q*i  < dims[!q] ) //test j when q = 0 and i when q = 1
							{
								if( print_info ) std::cout << "front cell: " << i << "," << j << std::endl;
								for(int k = 0; k < dims[2]; ++k)
								{
									for(int l = 0; l < 2; ++l) //top-bottom
									{
										//q = 0 - test 0,1 + 4*l (edge 0 and 2)
										//q = 1 - test 0,2 + 4*l (edge 4 and 6)
										edge_nodes[0] = p0[zcorn_array[i][j][k][0 + 0 + 4*l]];
										edge_nodes[1] = p1[zcorn_array[i][j][k][1 + q + 4*l]];
										if( edge_nodes[0] != edge_nodes[1] )
										{
											edge_nodes.SetMarker(used);
											Edge e = CreateEdge(edge_nodes).first;
											if( !e->GetMarker(visited) )
											{
												edges.push_back(e);
												e->SetMarker(visited);
											}
											e->IntegerArray(block_number).push_back((i + (j+k*dims[1])*dims[0]));
											e->IntegerArray(edge_number).push_back(0 + 2*l + 4*q);
											Storage::integer_array mark = e->IntegerArray(pillar_mark);
											//front block indices
											mark[0] = i;
											mark[1] = j;
											//nx or ny
											mark[2] = q;
										}
									}
								}
							}
							edges.RemMarker(visited);
							ReleaseMarker(visited);
							
							if( i == 43 && j == 41 && q == 1)
								print_inter = true;
							
							//produce intersected edges
							if( print_inter )
							{
								std::cout << "input edges: " << edges.size() << std::endl;
								for(int k = 0; k < edges.size(); ++k)
								{
									Storage::integer_array bn = edges[k]->IntegerArray(block_number);
									Storage::integer_array en = edges[k]->IntegerArray(edge_number);
									std::cout << "edge " << k << " " << edges[k]->GetHandle() << " " << edges[k]->getBeg()->GetHandle() << "<->" << edges[k]->getEnd()->GetHandle() << " blocks: ";
									for(int l = 0; l < bn.size(); ++l)
										std::cout << bn[l] << ":" << en[l] << " ";
									std::cout << std::endl;
								}
							}
							assert(count_duplicates(edges) == 0);
							//i << "," << j << " and " << i+!q << "," << j+q <<
							intersect(this,edges,intersections,transfer,&coords_array[i][j][0][0],&coords_array[i][j][1][0],&coords_array[i+!q][j+q][0][0],&coords_array[i+!q][j+q][1][0],print_inter);
							assert(count_duplicates(edges) == 0);
							if(!intersections.empty()) std::cout << "intersections: " << intersections.size() << std::endl;
							if( print_inter )
							{
								
								std::cout << "output edges: " << edges.size() << std::endl;
								for(int k = 0; k < edges.size(); ++k)
								{
									std::cout << "edge " << k << " " << edges[k]->getBeg()->GetHandle() << "<->" << edges[k]->getEnd()->GetHandle() << std::endl;
								}
							}
							
							if( i == 43 && j == 41 && q == 1)
								print_inter = false;
							
							//distribute all the edges among blocks
							//add intersected edges into blocks, so that we can reconstruct top and bottom faces of each block
							for(int k = 0; k < (int)edges.size(); ++k)
							{
								Storage::integer_array b = edges[k].IntegerArray(block_number);
								for(int r = 0; r < (int)b.size(); ++r)
									block_edges[b[r]].push_back(edges[k]);
							}
							//add vertical edges along pillars
							ElementArray<Edge> & p0_edges = pillar_edges[(i+ 0)*(dims[1]+1)+j+0];
							ElementArray<Edge> & p1_edges = pillar_edges[(i+!q)*(dims[1]+1)+j+q];
							edges.reserve(edges.size()+p0_edges.size()+p1_edges.size());
							for(int k = 0; k < (int)p0_edges.size(); ++k)
							{
								if( p0_edges[k]->nbAdjElements(NODE,used) == 2 ) //this edge is used
									edges.push_back(p0_edges[k]);
							}
							for(int k = 0; k < (int)p1_edges.size(); ++k)
							{
								if( p1_edges[k]->nbAdjElements(NODE,used) == 2 ) //this edge is used
									edges.push_back(p1_edges[k]);
							}
							assert(count_duplicates(edges) == 0);
							//sort block numbers on edges
							for(int k = 0; k < (int)edges.size(); ++k)
							{
								Storage::integer_array b = edges[k].IntegerArray(block_number);
								Storage::integer_array e = edges[k].IntegerArray(edge_number);
								assert(e.size() == b.size());
								//sort indices according to b
								indices_sort.resize(b.size());
								for(int l = 0; l < indices_sort.size(); ++l) indices_sort[l] = l;
								std::sort(indices_sort.begin(),indices_sort.end(),index_comparator(b));
								//arrange data in b and e arrays according to indices_sort
								temporary.resize(b.size());
								//first b array
								for(int l = 0; l < b.size(); ++l) temporary[l] = b[l];
								for(int l = 0; l < b.size(); ++l) b[l] = temporary[indices_sort[l]];
								//then e array
								for(int l = 0; l < e.size(); ++l) temporary[l] = e[l];
								for(int l = 0; l < e.size(); ++l) e[l] = temporary[indices_sort[l]];
							}
							//put block numbers to all nodes involved in pillars, so that we can figure out block numbers for constructed faces
							edges.SetMarker(mrk);
							//report block numbers on edges
							if(print_bn)
							{
								for(int k = 0; k < (int)edges.size(); ++k)
								{
									Storage::integer_array b = edges[k]->IntegerArray(block_number);
									Storage::integer_array e = edges[k]->IntegerArray(edge_number);
									std::cout << "edge " << k << " " << edges[k]->GetHandle() << " blocks [" << b.size() << "]: ";
									for(int l = 0; l < (int)b.size()-1; ++l) std::cout << b[l] << ":" << e[l] << " ";
									std::cout << b.back() << ":" << e.back() << std::endl;
								}
							}
							//unite block numbers on nodes
							if( print_bn ) std::cout << "pillar 0 nodes: " << std::endl;
							for(pillar::iterator it = p0.begin(); it != p0.end(); ++it) if( it->second.GetMarker(used) )
							{
								ElementArray<Element> nedges = it->second->getAdjElements(EDGE,mrk);
								block_number_union(it->second.getAsElement(),nedges,block_number);
								if( print_bn )
								{
									Storage::integer_array b = it->second->IntegerArray(block_number);
									std::cout << "node " << it->second->GetHandle() << " blocks [" << b.size() << "]: ";
									for(int l = 0; l < (int)b.size()-1; ++l) std::cout << b[l] << ", ";
									std::cout << b.back() << std::endl;
								}
							}
							if( print_bn ) std::cout << "pillar 1 nodes: " << std::endl;
							for(pillar::iterator it = p1.begin(); it != p1.end(); ++it) if( it->second.GetMarker(used) )
							{
								ElementArray<Element> nedges = it->second->getAdjElements(EDGE,mrk);
								block_number_union(it->second.getAsElement(),nedges,block_number);
								if( print_bn )
								{
									Storage::integer_array b = it->second->IntegerArray(block_number);
									std::cout << "node " << it->second->GetHandle() << " blocks [" << b.size() << "]: ";
									for(int l = 0; l < (int)b.size()-1; ++l) std::cout << b[l] << ", ";
									std::cout << b.back() << std::endl;
								}
							}
							if( print_bn ) std::cout << "intersection nodes: " << std::endl;
							for(int k = 0; k < (int)intersections.size(); ++k)
							{
								ElementArray<Element> nedges = intersections[k]->getAdjElements(EDGE,mrk);
								block_number_union(intersections[k].getAsElement(),nedges,block_number);
								if( print_bn )
								{
									Storage::integer_array b = intersections[k]->IntegerArray(block_number);
									std::cout << "node " << k << " " << intersections[k]->GetHandle() << " blocks [" << b.size() << "]: ";
									for(int l = 0; l < (int)b.size()-1; ++l) std::cout << b[l] << ", ";
									std::cout << b.back() << std::endl;
								}
							}
							//unmark nodes
							for(pillar::iterator it = p0.begin(); it != p0.end(); ++it)  it->second.RemMarker(used);
							for(pillar::iterator it = p1.begin(); it != p1.end(); ++it)  it->second.RemMarker(used);
							//unmark edges
							edges.RemMarker(mrk);
							//distribute edges to front and back blocks, so that we can assemble faces
							for(int k = 0; k < (int)edges.size(); ++k)
							{
								//intersect block numbers on nodes to detect to which blocks this edge belongs
								ElementArray<Element> nodes = edges[k]->getAdjElements(NODE);
								block_number_intersection(nodes,block_number,block_number_inter);
								for(int m = 0; m < (int)block_number_inter.size(); ++m)
								{
									int bi = block_number_inter[m] % dims[0];
									int bj = block_number_inter[m] / dims[0] % dims[1];
									int bk = block_number_inter[m] / dims[0] / dims[1];
									assert(bi >= 0 && bi < dims[0]);
									assert(bj >= 0 && bj < dims[1]);
									if( bi == i-q && bj == j-!q ) //back blocks
									{
										pillar_block_edges_back[bk].push_back(edges[k]);
										if( print_bn ) std::cout << "add edge " << k << " " << edges[k]->GetHandle() << " to back  block " << block_number_inter[m] << " (" << bi << "," << bj << "," << bk << ")" << std::endl;
									}
									else if(bi == i && bj == j) //front blocks
									{
										pillar_block_edges_front[bk].push_back(edges[k]);
										if( print_bn ) std::cout << "add edge " << k << " " << edges[k]->GetHandle() << " to front block " << block_number_inter[m] << " (" << bi << "," << bj << "," << bk << ")" << std::endl;
									}
									//skip non-current blocks
								}
							}
							std::vector< ElementArray<Edge> > * pillar_block_edges[2] = {&pillar_block_edges_back,&pillar_block_edges_front};
							//(i-q + (j-!q+k*dims[1])*dims[0])
							int blocki[2] = {i-q,i};
							int blockj[2] = {j-!q,j};
							if( print_info )
							{
								std::cout << "back  block " << blocki[0] << "," << blockj[0] << std::endl;
								std::cout << "front block " << blocki[1] << "," << blockj[1] << std::endl;
								std::cout << "dims " << dims[0] << "," << dims[1] << std::endl;
							}
							//construct interfaces (some computations will be redundant)
							for(int m = 0; m < 2; ++m) //back and front
							{
								if( blocki[m] == -1 || blockj[m] == -1 ) {if(print_info) std::cout << "skip " << (m?"front ":"back ") << std::endl; continue;}
								if( blocki[m] == dims[0] || blockj[m] == dims[1] ) {if(print_info) std::cout << "skip " << (m?"front ":"back ") << std::endl; continue;}
								if( print_bedges ) std::cout << (m?"front ":"back ") << " column of blocks: " << blocki[m] << "," << blockj[m] << std::endl;
								for(int k = 0; k < dims[2]; ++k) //go down the piller
								{
									//retrive edge for the side
									ElementArray<Edge> & bedges = pillar_block_edges[m]->at(k);
									if( bedges.empty() ) continue;
									//remove any duplicates
									make_unique(bedges);
									int num_outer = 0;
									std::set<int> outer_edge_number;
									for(int l = 0; l < bedges.size(); ++l) //loop through edges of the block
									{
										//retrive block numbers of edges
										Storage::integer_array bn = bedges[l]->IntegerArray(block_number);
										Storage::integer_array en = bedges[l]->IntegerArray(edge_number);
										for(int r = 0; r < (int)bn.size(); ++r)
										{
											if( bn[r] == blocki[m] + (blockj[m]+k*dims[1])*dims[0] ) //this edge originally created by the block
											{
												bedges[l]->SetMarker(outer); //mark edge
												num_outer++;
												outer_edge_number.insert(en[r]);
											}
										}
									}
									//move marked edges to the end
									if( outer_edge_number.size() > 1 )
									{
										std::sort(bedges.begin(),bedges.end(),Mesh::MarkerComparator(this,outer));
										if( print_bedges )
										{
											std::cout << (m?"front ":"back ") << "depth " << k << " block " <<blocki[m] + (blockj[m]+k*dims[1])*dims[0] << " edges [" << bedges.size() << "]:" << std::endl;
											for(int l = 0; l < bedges.size(); ++l)
											{
												Storage::integer_array bn = bedges[l]->IntegerArray(block_number);
												Storage::integer_array en = bedges[l]->IntegerArray(edge_number);
												std::cout << "edge " << l << " " << bedges[l]->GetHandle() << " " << (bedges[l]->GetMarker(outer) ? "outer":"inner");
												std::cout << " blocks ";
												for(int r = 0; r < bn.size(); ++r)
													std::cout << bn[r] << ":" << en[r] << " ";
												std::cout << std::endl;
												
											}
											assert(count_duplicates(bedges) == 0);
										}
										//remove marker
										bedges.RemMarker(outer);
										//form faces out of edges
										incident_matrix<Edge> matrix(this,bedges.data(),bedges.data()+bedges.size(),bedges.size()-num_outer);
										//collect all faces
										ElementArray<Edge> loop(this);
										while(matrix.find_shortest_loop(loop))
										{
											if( loop.size() > 2 ) //at least triangle
											{
												if( print_bedges )
												{
													std::cout << "Found loop of " << loop.size() << " edges:" <<std::endl;
													for(int g = 0; g < loop.size(); ++g)
													{
														std::cout << "edge " << g << " " << loop[g]->GetHandle() << " ";
														Storage::integer_array bn = loop[g]->IntegerArray(block_number);
														Storage::integer_array en = loop[g]->IntegerArray(edge_number);
														std::cout << " blocks ";
														for(int r = 0; r < bn.size(); ++r)
															std::cout << bn[r] << ":" << en[r] << " ";
														std::cout << std::endl;
													}
													
												}
												//make face
												Face f = CreateFace(loop).first;
												if(!f->CheckEdgeOrder()) std::cout << __FILE__ << ":" << __LINE__ << " bad edge order, edges " << loop.size() << std::endl;
												//detect block number
												ElementArray<Element> nodes = f->getAdjElements(NODE);
												if( print_bedges )
												{
													std::cout << "nodes of the face " << f->GetHandle() << ": " << std::endl;
													for(int g = 0; g < nodes.size(); ++g)
													{
														std::cout << "node " << g << " " << nodes[g]->GetHandle() << " ";
														Storage::integer_array bn = nodes[g]->IntegerArray(block_number);
														std::cout << " blocks ";
														for(int r = 0; r < bn.size(); ++r)
															std::cout << bn[r] << " ";
														std::cout << std::endl;

													}
													std::cout << "intersection: ";
												}
												block_number_intersection(nodes,block_number,block_number_inter);
												for(int r = 0; r < (int)block_number_inter.size(); ++r)
												{
													if( print_bedges ) std::cout << block_number_inter[r] << " (" << block_number_inter[r]%dims[0] << "," << block_number_inter[r]/dims[0] % dims[1] << "," << block_number_inter[r]/dims[0]/dims[1] <<") ";
													block_faces[block_number_inter[r]].push_back(f);
												}
												if( print_bedges ) std::cout << std::endl;
											}
											else if( !loop.empty() ) std::cout << __FILE__ << ":" << __LINE__ << " skip degenerate face " << loop.size() << std::endl;
										}
										//if( !matrix.all_visited() ) matrix.print_matrix();
									}
									//cleanup structure for reuse
									bedges.clear();
								}
							}
							//clean-up structures
							edges.clear();
							intersections.clear();
						} //j
					} //i
					printf("\n");
				} //q
				ReleaseMarker(used);
				ReleaseMarker(mrk);
				ReleaseMarker(outer);
				//do not need this tag on nodes
				//DeleteTag(block_number, NODE);
				//now construct top and bottom interfaces
				for(int i = 0; i < dims[0]; ++i)
				{
					printf("top/bottom/cells %6.2f%%\r", ((double)i)/((double)dims[0]-1)*100);
					fflush(stdout);
					for(int j = 0; j < dims[1]; ++j)
					{
						for(int k = 0; k < dims[2]; ++k)
						{
							//current block number
							int cur = (i + (j+k*dims[1])*dims[0]);
							//bottom face -> 0,4,1,5
							//top face -> 2,7,3,6
							const int map[8][2] =
							{
								{0,0}, //edge 0 -> first position, bottom face
								{2,0}, //edge 1 -> third position, bottom face
								{0,1}, //edge 2 -> first position, top face
								{2,1}, //edge 3 -> third position, top face
								{1,0}, //edge 4 -> second position, bottom face
								{3,0}, //edge 5 -> fourth position, bottom face
								{3,1}, //edge 6 -> fourth position, top face
								{1,1} //edge 7 -> second position, top face
							};
							//which edges should be considered in reverse order
							const bool rev[2][4] = 
							{
								{true,false,false,true},
								{false,false,true,true}
							};
							//array of ordered edges, first dimension - top or bottom, second dimension - edge number
							std::vector<HandleType> edges[2][4];
							//retrive set of edges of the block
							ElementArray<Edge> & cbe = block_edges[cur];
							for(int q = 0; q < cbe.size(); ++q)
							{
								Storage::integer_array bn = cbe[q].IntegerArray(block_number);
								Storage::integer_array en = cbe[q].IntegerArray(edge_number);
								assert(bn.size() == en.size());
								for(int r = 0; r < (int)bn.size(); ++r) if( bn[r] == cur ) //how the edge is represented on current block
									edges[map[en[r]][1]][map[en[r]][0]].push_back(cbe[q].GetHandle());
							}
							ElementArray<Edge> face_edges(this);
							//collect edges and create face
							for(int q = 0; q < 2; ++q) //top and bottom
							{
								for(int r = 0; r < 4; ++r) //all sides
								{
									if( rev[q][r] )
										face_edges.insert(face_edges.end(),edges[q][r].rbegin(),edges[q][r].rend());
									else
										face_edges.insert(face_edges.end(),edges[q][r].begin(),edges[q][r].end());
								}
								make_unique(face_edges); //somehow duplicate appears there
								if( face_edges.size() > 2 )
								{
									Face f = CreateFace(face_edges).first;
									
									if(!f->CheckEdgeOrder())
									{
										std::cout << __FILE__ << ":" << __LINE__ << " bad edge order, edges " << face_edges.size() << std::endl;
										for(int l = 0; l < face_edges.size(); ++l)
										{
											//Storage::integer_array bn = face_edges[l]->IntegerArray(block_number);
											//Storage::integer_array en = face_edges[l]->IntegerArray(edge_number);
											std::cout << "edge " << l << " " << face_edges[l]->GetHandle() << " ";
											//std::cout << "blocks: ";
											//for(int p = 0; p < bn.size(); ++p)
											//	std::cout << bn[p] << ":" << en[p] << " ";
											std::cout << std::endl;
										}
									}
									block_faces[cur].push_back(f);
								}
								else if( !face_edges.empty() ) std::cout << __FILE__ << ":" << __LINE__ << " skip degenerate face " << face_edges.size() << " " << (q?"top":"bottom") << " of block " << cur << " (" << i << "," << j << "," << k << ") actnum ma" << actnum[ECL_IJK_DATA(i,j,k)] << std::endl;
								face_edges.clear();
							}
							make_unique(block_faces[cur]); //some faces may be added twice?
							if( block_faces[cur].size() > 3 )
								CreateCell(block_faces[cur]).first->Integer(cell_number) = cur;
							else if( !block_faces[cur].empty() )
							{
								//depending on actnum, mark faces that are not connected
								//std::cout << __FILE__ << ":" << __LINE__ << " skip degenerate cell " << block_faces[cur].size() << " block " << cur << " (" << i << "," << j << "," << k << ") actnum " << actnum[ECL_IJK_DATA(i,j,k)] << std::endl;
							}
						} //k
					} //j
				} //i
				printf("\n");
				//cleanup data
				//DeleteTag(edge_number);
				//DeleteTag(block_number);
				//crack up the mesh along degenerate active cells
				//populate properties to blocks
			}
		}
	}
}

#endif