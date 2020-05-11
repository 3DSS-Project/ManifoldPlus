#include "MeshProjector.h"

#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>

#include <Eigen/Dense>

#include <igl/per_vertex_normals.h>
#include <igl/per_face_normals.h>
#include <igl/point_mesh_squared_distance.h>

#include "Intersection.h"
#include "IO.h"

#define ZERO_THRES 1e-9
MeshProjector::MeshProjector()
{}

void MeshProjector::ComputeHalfEdge()
{
	V2E_.resize(num_V_);
	E2E_.resize(num_F_ * 3);

	for (int i = 0; i < num_V_; ++i)
		V2E_[i] = -1;
	for (int i = 0; i < num_F_ * 3; ++i)
		E2E_[i] = -1;

	std::map<std::pair<int, int>, int> dedges;
	for (int i = 0; i < num_F_; ++i) {
		for (int j = 0; j < 3; ++j) {
			int v0 = out_F_(i, j);
			int v1 = out_F_(i, (j + 1) % 3);
			V2E_[v0] = i * 3 + j;
			auto k = std::make_pair(v1, v0);
			auto it = dedges.find(k);
			if (it == dedges.end()) {
				dedges[std::make_pair(v0, v1)] = i * 3 + j;
			} else {
				int rid = it->second;
				E2E_[i * 3 + j] = rid;
				E2E_[rid] = i * 3 + j;
			}
		}
	}
#ifdef DEBUG_
	for (int i = 0; i < num_V_; ++i) {
		if (V2E_[i] == -1) {
			printf("independent vertex! %d\n", i);
			exit(0);
		}
	}
	for (int i = 0; i < num_F_ * 3; ++i) {
		if (E2E_[i] == -1) {
			printf("Wrong edge!\n");
			exit(0);
		}
		if (E2E_[E2E_[i]] != i) {
			printf("Wrong edge 2!\n");
			exit(0);
		}
	}
#endif
}

void MeshProjector::SplitVertices() {
	std::vector<std::unordered_set<int> > vlinks(num_V_);
	for (int i = 0; i < num_F_; ++i) {
		for (int j = 0; j < 3; ++j) {
			int v0 = out_F_(i, j);
			vlinks[v0].insert(i * 3 + j);
		}
	}
	int invalid_vertex = 0;
	std::vector<std::pair<int, int> > insert_vertex_info;
	int num_vertices = num_V_;
	for (int i = 0; i < num_V_; ++i) {
		int deid = V2E_[i];
		int deid0 = deid;
		int vertex_count = 0;
		do {
			vertex_count += 1;
			deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
		} while (deid0 != deid);
		if (vertex_count != vlinks[i].size()) {
			int group_id = 0;
			invalid_vertex += 1;
			while (!vlinks[i].empty()) {
				int deid = *vlinks[i].begin();
				int deid0 = deid;
				std::vector<int> dedges;
				do {
					dedges.push_back(deid);
					deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
				} while (deid0 != deid);
				for (auto& p : dedges)
					vlinks[i].erase(p);

				if (group_id != 0) {
					insert_vertex_info.push_back(
						std::make_pair(num_vertices, i));
					for (auto& p : dedges) {
						out_F_(p / 3, p % 3) = num_vertices;
					}
					num_vertices += 1;
				}
				group_id += 1;
			}
		}
	}
	out_V_.conservativeResize(num_vertices, 3);
	for (auto& p : insert_vertex_info) {
		out_V_.row(p.first) = out_V_.row(p.second);
	}
	num_V_ = num_vertices;
}

void MeshProjector::ComputeIndependentSet() {
	int marked_vertices = 0;
	int group_id = 0;
	std::vector<int> vertex_colors(num_V_, -1);
	while (marked_vertices < num_V_) {
		for (int i = 0; i < vertex_colors.size(); ++i) {
			vertex_groups_.push_back(std::vector<int>());
			auto& group = vertex_groups_.back();
			if (vertex_colors[i] != -1)
				continue;
			int deid = V2E_[i];
			int deid0 = deid;
			bool conflict = false;
			do {
				int next_v = out_F_(deid / 3, (deid + 1) % 3);
				if (vertex_colors[next_v] == group_id) {
					conflict = true;
					break;
				}
				deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
			} while (deid0 != deid);
			if (!conflict) {
				vertex_colors[i] = group_id;
				group.push_back(i);
				marked_vertices += 1;
			}
			std::random_shuffle(group.begin(), group.end());
		}
		group_id += 1;
	}	
}

void MeshProjector::Project(const MatrixD& V, const MatrixI& F,
	MatrixD* out_V, MatrixI* out_F)
{
	V_ = V;
	F_ = F;
	out_V_ = *out_V;
	out_F_ = *out_F;

	FT len = (V.row(out_F_(0,0)) - V.row(out_F_(0,1))).norm();

	num_F_ = out_F_.rows();
	num_V_ = out_V_.rows();

	tree_.init(V_,F_);

	ComputeHalfEdge();
	SplitVertices();
	ComputeHalfEdge();
	ComputeIndependentSet();
	IterativeOptimize(len, false);
	AdaptiveRefine(len, 1e-3);

	out_V->conservativeResize(num_V_, 3);
	out_F->conservativeResize(num_F_, 3);
	for (int i = 0; i < num_V_; ++i) {
		out_V->row(i) = out_V_.row(i);
	}
	for (int i = 0; i < num_F_; ++i) {
		out_F->row(i) = out_F_.row(i);
	}
}

void MeshProjector::UpdateNearestDistance()
{
	//igl::point_mesh_squared_distance(out_V_, V_, F_, sqrD_, I_, target_V_);
	tree_.squared_distance(V_,F_,out_V_,sqrD_,I_,target_V_);
}

void MeshProjector::UpdateFaceNormal(int i)
{
	int deid = V2E_[i];
	int deid0 = deid;
	do {
		int f = deid / 3;
		int v0 = out_F_(f, deid % 3);
		int v1 = out_F_(f, (deid + 1) % 3);
		int v2 = out_F_(f, (deid + 2) % 3);
		Vector3 d0 = out_V_.row(v1) - out_V_.row(v0);
		Vector3 d1 = out_V_.row(v2) - out_V_.row(v0);
		d0.normalize();
		d1.normalize();
		auto vn = d0.cross(d1);		
		double l = vn.norm();
		if (l > 0)
			vn /= l;
		out_FN_.row(f) = vn;
	} while (deid0 != deid);
}

void MeshProjector::UpdateVertexNormal(int i, int conservative) {
	int deid = V2E_[i];
	int deid0 = deid;
	Vector3 n(0,0,0);
	do {
		int f = deid / 3;
		int v0 = out_F_(f, deid % 3);
		int v1 = out_F_(f, (deid + 1) % 3);
		int v2 = out_F_(f, (deid + 2) % 3);
		Vector3 d0 = out_V_.row(v1) - out_V_.row(v0);
		Vector3 d1 = out_V_.row(v2) - out_V_.row(v0);
		d0.normalize();
		d1.normalize();
		auto vn = d0.cross(d1);
		double l = vn.norm();
		vn = vn * (asin(l) / l);
		n += vn;
		deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
	} while (deid0 != deid);

	if (conservative) {
		do {
			int f = deid / 3;
			int v0 = out_F_(f, deid % 3);
			int v1 = out_F_(f, (deid + 1) % 3);
			int v2 = out_F_(f, (deid + 2) % 3);
			Vector3 d0 = out_V_.row(v1) - out_V_.row(v0);
			Vector3 d1 = out_V_.row(v2) - out_V_.row(v0);
			auto vn = d0.cross(d1).normalized();
			if (n.dot(vn) < 0) {
				n -= n.dot(vn) * vn;
			}
			deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
		} while (deid0 != deid);
	}

	out_N_.row(i) = n.normalized();
}

void MeshProjector::UpdateVertexNormals(int conservative)
{
	if (out_N_.rows() < num_V_)
		out_N_.resize(num_V_, 3);
	for (int i = 0; i < num_V_; ++i) {
		UpdateVertexNormal(i, conservative);
	}
}

int MeshProjector::BoundaryCheck() {
	igl::per_face_normals(out_V_, out_F_, out_FN_);
	int consistent = 0;
	int inconsistent = 0;
	for (int i = 0; i < num_V_; ++i) {
		Vector3 n = out_N_.row(i);
		int deid = V2E_[i];
		int deid0 = deid;
		do {
			Vector3 fn = out_FN_.row(deid / 3);
			if (n.dot(fn) < -ZERO_THRES) {
				inconsistent += 1;
				printf("%d %d %f: <%f %f %f> <%f %f %f>\n",
					i, deid / 3, n.dot(fn),
					n[0], n[1], n[2],
					fn[0], fn[1], fn[2]);
			}
			else
				consistent += 1;
			deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
		} while (deid0 != deid);
	}
	return inconsistent;
}

void MeshProjector::IterativeOptimize(FT len, bool initialized) {
	if (!initialized) {
		indices_.resize(num_V_);
		UpdateVertexNormals(1);
		UpdateNearestDistance();
		igl::per_face_normals(out_V_, out_F_, out_FN_);
		active_vertices_.resize(num_V_);
		active_vertices_temp_.resize(num_V_);
		for (int i = 0; i < num_V_; ++i) {
			active_vertices_[i] = i;
		}
		num_active_ = num_V_;
	}

	int iter = 0;
	while (true) {
		/*
		for (int i = 0; i < sharp_vertices_.size(); ++i) {
			if (sharp_vertices_[i] > 0) {
				sqrD_[i] = 10000 * sharp_vertices_[i] + (sharp_positions_[i]
					- Vector3(out_V_.row(i))).squaredNorm();
				target_V_.row(i) = sharp_positions_[i];
				out_V_.row(i) = sharp_positions_[i];
			}
		}
		*/
		for (int i = 0; i < num_active_; ++i) {
			int vid = active_vertices_[i];
			indices_[i] = std::make_pair(sqrD_[vid], vid);
		}
		bool changed = false;
		std::sort(indices_.begin(), indices_.begin() + num_active_);
		double max_change = 0;
		int num_active_temp = 0;

		for (int i = num_active_ - 1; i >= 0; --i) {
			int vid = indices_[i].second;

			double d0 = (out_V_.row(vid) - target_V_.row(vid)).norm();
			OptimizePosition(vid, target_V_.row(vid), len);
			double d1 = (out_V_.row(vid) - target_V_.row(vid)).norm();

			UpdateFaceNormal(vid);
			auto vn = out_N_.row(vid);

			UpdateVertexNormal(vid, 0);
			OptimizeNormal(vid, vn, out_N_.row(vid));

			if (std::abs(d1 - d0) > ZERO_THRES
				|| vn.dot(out_N_.row(vid)) < 1 - ZERO_THRES)
			{
				if (std::abs(d1 - d0) > 1e-6)
					changed = true;
				if (std::abs(d1 - d0) > std::abs(max_change))
					max_change = d1 - d0;
				active_vertices_temp_[num_active_temp++] = vid;
			}
		}

		if (num_active_temp > out_V_.rows() / 2)
			UpdateNearestDistance();
		else {
			MatrixD P(num_active_temp, 3);
			for (int i = 0; i < num_active_temp; ++i) {
				P.row(i) = out_V_.row(active_vertices_temp_[i]);
			}
			MatrixD targetP;
			VectorX sqrD;
			VectorXi I;
			tree_.squared_distance(V_,F_,P,sqrD,I,targetP);

			for (int i = 0; i < num_active_temp; ++i) {
				target_V_.row(active_vertices_temp_[i]) = targetP.row(i);
				sqrD_[active_vertices_temp_[i]] = sqrD[i];
				I_[active_vertices_temp_[i]] = I[i];
			}
		}

		std::unordered_set<int> novel_activate;
		for (int i = 0; i < num_active_temp; ++i) {
			int p = active_vertices_temp_[i];
			novel_activate.insert(p);
			int deid = V2E_[p];
			int deid0 = deid;
			do {
				novel_activate.insert(out_F_(deid / 3, (deid + 1) % 3));
				deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
			} while (deid0 != deid);
		}

		num_active_ = 0;
		for (auto& p : novel_activate)
			active_vertices_[num_active_++] = p;
		/*
		if (iter == 2)
			PreserveSharpFeatures(len);
		*/
		if (!changed)
			break;
		iter += 1;
	}

}

void MeshProjector::Highlight(int id, FT len) {
	UpdateNearestDistance();
	double max_dis = 0;
	std::vector<FT> distances(sqrD_.size());
	memcpy(distances.data(), sqrD_.data(), sizeof(FT) * sqrD_.size());
	for (auto& d : distances)
		d = sqrt(d);
	std::sort(distances.rbegin(), distances.rend());
	max_dis = distances[0];
	printf("Max distance %lf\n", max_dis / len);
 	char buffer[1024];
 	sprintf(buffer, "%05d-tri.obj", id);
 	WriteOBJ(buffer, out_V_, out_F_);
 	sprintf(buffer, "%05d-point.obj", id);
 	std::ofstream os(buffer);
 	for (int i = 0; i < sqrD_.size(); ++i) {
 		double dis = sqrt(sqrD_[i]);
 		if (dis > max_dis - 1e-7) {
 			printf("Id %d\n", i);
 			Vector3 d1 = target_V_.row(i) - out_V_.row(i);
 			printf("Distance0 %lf\n", d1.norm());
 			OptimizePosition(i, target_V_.row(i), len, true);
 			Vector3 d2 = target_V_.row(i) - out_V_.row(i);
 			printf("Distance1 %lf\n", d2.norm());
 			Vector3 v = out_V_.row(i);
 			Vector3 n = out_N_.row(i);
 			os << "v " << v[0] << " " << v[1] << " " << v[2] << " 0 0.99 0\n";
 			v += n * 1e-3;
 			os << "v " << v[0] << " " << v[1] << " " << v[2] << " 0.99 0 0\n";
 			Vector3 p = target_V_.row(i);
 			os << "v " << p[0] << " " << p[1] << " " << p[2] << " 0.99 0.99 0\n";
 		}
 	}
 	os.close();
}

void MeshProjector::AdaptiveRefine(FT len, FT ratio) {
	std::vector<int> candidates;
	candidates.reserve(num_F_ * 3 / 2);
	for (int i = 0; i < num_F_ * 3; ++i) {
		if (E2E_[i] > i)
			candidates.push_back(i);
	}

	double max_dis = 0;
	for (int i = 0; i < num_V_; ++i) {
		double dis = sqrt(sqrD_[i]);
		if (dis > max_dis)
			max_dis = dis;
	}
	
	auto AddVertex = [&](const Vector3& p, const Vector3& n,
		const Vector3& tar_p, const FT& sqr_dis, int face_index) {
		if (num_V_ >= out_V_.rows()) {
			out_V_.conservativeResize(out_V_.rows() * 2, 3);
			out_N_.conservativeResize(out_N_.rows() * 2, 3);
			target_V_.conservativeResize(target_V_.rows() * 2, 3);
			sqrD_.conservativeResize(sqrD_.rows() * 2);
			I_.conservativeResize(I_.size() * 2);
			indices_.resize(indices_.size() * 2);
			active_vertices_.resize(active_vertices_.size() * 2);
			active_vertices_temp_.resize(active_vertices_temp_.size() * 2);
		}
		out_V_.row(num_V_) = p;
		out_N_.row(num_V_) = n;
		target_V_.row(num_V_) = tar_p;
		sqrD_[num_V_] = sqr_dis;
		I_[num_V_] = face_index;
		num_V_ += 1;
	};

	auto AddFace = [&](const Vector3i& f, const Vector3& n) {
		if (num_F_ >= out_F_.rows()) {
			out_F_.conservativeResize(out_F_.rows() * 2, 3);
			out_FN_.conservativeResize(out_FN_.rows() * 2, 3);
		}
		out_F_.row(num_F_) = f;
		out_FN_.row(num_F_) = n;
		num_F_ += 1;
	};

	for (int iter = 0; iter < 4; ++iter) {
		// Collect dedges to split
		MatrixD P(candidates.size(), 3);
		MatrixD targetP;

		for (int i = 0; i < candidates.size(); ++i) {
			int deid = candidates[i];
			int v0 = out_F_(deid / 3, deid % 3);
			int v1 = out_F_(deid / 3, (deid + 1) % 3);

			P.row(i) = (out_V_.row(v0) + out_V_.row(v1)) * 0.5;
		}
		//igl::point_mesh_squared_distance(P, V_, F_, sqrD_, I_, targetP);
		VectorXi I;
		VectorX sqrD;
		tree_.squared_distance(V_,F_,P,sqrD,I,targetP);
		int top = 0;
		double max_dis = 0;
		for (int i = 0; i < sqrD.size(); ++i) {
			double dis = sqrt(sqrD[i]);
			if (dis > len * ratio) {
				P.row(top) = P.row(i);
				targetP.row(top) = targetP.row(i);
				sqrD[top] = sqrD[i];
				I[top] = I[i];
				candidates[top++] = candidates[i];
			}
			if (dis > max_dis)
				max_dis = dis;
		}
		candidates.resize(top);

		int prev_vertex_num = num_V_;
		int prev_face_num = num_F_;

		// insert vertices
		std::map<int, Vector3i > face_splits;
		for (int i = 0; i < top; ++i) {
			int deid = candidates[i];
			for (int j = 0; j < 2; ++j) {
				int f = deid / 3;
				auto it = face_splits.find(f);
				if (it == face_splits.end()) {
					Vector3i v(-1, -1, -1);
					v[deid % 3] = num_V_;
					face_splits[f] = v;
				} else {
					it->second[deid % 3] = num_V_;
				}
				deid = E2E_[deid];
			}

			int v0 = out_F_(deid / 3, deid % 3);
			AddVertex(P.row(i), out_N_.row(v0), targetP.row(i), sqrD[i], I[i]);
		}

		// insert faces
		std::map<std::pair<int, int>, int> dedges;
		for (auto& info : face_splits) {
			int f = info.first;
			Vector3 fn = out_FN_.row(f);
			auto splits = info.second;
			int count = 0;
			for (int j = 0; j < 3; ++j) {
				if (splits[j] >= 0)
					count += 1;
			}
			if (count == 3) {
				int v0 = out_F_(f, 0);
				int v1 = out_F_(f, 1);
				int v2 = out_F_(f, 2);

				int nv0 = splits[0];
				int nv1 = splits[1];
				int nv2 = splits[2];

				out_F_.row(f) = Vector3i(v0, nv0, nv2);

				AddFace(Vector3i(nv0, nv1, nv2), fn);
				AddFace(Vector3i(nv0, v1, nv1), fn);
				AddFace(Vector3i(nv2, nv1, v2), fn);
			}
			else if (count == 2) {
				int j = 0;
				while (splits[j] != -1) {
					j += 1;
				}
				int v0 = out_F_(f, j);
				int v1 = out_F_(f, (j + 1) % 3);
				int v2 = out_F_(f, (j + 2) % 3);
				int nv0 = splits[(j + 1) % 3];
				int nv1 = splits[(j + 2) % 3];

				dedges[std::make_pair(v1, v0)] = E2E_[f * 3 + j];

				out_F_.row(f) = Vector3i(v0, v1, nv0);
				AddFace(Vector3i(v0, nv0, nv1), fn);
				AddFace(Vector3i(nv1, nv0, v2), fn);
			}
			else if (count == 1) {
				int j = 0;
				while (splits[j] == -1) {
					j += 1;
				}
				int v0 = out_F_(f, j);
				int v1 = out_F_(f, (j + 1) % 3);
				int v2 = out_F_(f, (j + 2) % 3);

				dedges[std::make_pair(v2, v1)] = E2E_[f * 3 + (j + 1) % 3];
				dedges[std::make_pair(v0, v2)] = E2E_[f * 3 + (j + 2) % 3];

				int nv0 = splits[j];
				out_F_.row(f) = Vector3i(v0, nv0, v2);
				AddFace(Vector3i(nv0, v1, v2), fn);
			}
		}

		// insert E2E and V2E
		std::vector<int> update_face_set;
		std::set<int> update_vertex_set;
		update_face_set.reserve(face_splits.size() + num_F_ - prev_face_num);
		while (V2E_.size() < num_V_) {
			V2E_.conservativeResize(V2E_.size() * 2);
		}
		while (E2E_.size() < num_F_ * 3) {
			E2E_.conservativeResize(E2E_.size() * 2);
		}
		for (auto& info : face_splits) {
			auto f = out_F_.row(info.first);
			update_face_set.push_back(info.first);
			for (int i = 0; i < 3; ++i) {
				int v0 = f[i];
				int v1 = f[(i + 1) % 3];
				int dedge = info.first * 3 + i;
				V2E_[v0] = dedge;
				if (v0 < prev_vertex_num)
					update_vertex_set.insert(v0);
				dedges[std::make_pair(v0, v1)] = dedge;
			}
		}
		for (int k = prev_face_num; k < num_F_; ++k) {
			auto f = out_F_.row(k);
			update_face_set.push_back(k);
			for (int i = 0; i < 3; ++i) {
				int v0 = f[i];
				int v1 = f[(i + 1) % 3];
				int dedge = k * 3 + i;
				V2E_[v0] = dedge;
				if (v0 < prev_vertex_num)
					update_vertex_set.insert(v0);
				dedges[std::make_pair(v0, v1)] = dedge;
			}
		}
		for (auto& info : dedges) {
			int deid = info.second;
			auto key = std::make_pair(info.first.second, info.first.first);
			int rdeid = dedges[key];
			E2E_[deid] = rdeid;
			E2E_[rdeid] = deid;
		}

		// update candidates
		candidates.clear();
		for (auto& info : face_splits) {
			auto f = out_F_.row(info.first);
			for (int i = 0; i < 3; ++i) {
				int v0 = f[i];
				int v1 = f[(i + 1) % 3];
				if (v0 >= prev_vertex_num || v1 >= prev_vertex_num) {
					int dedge = info.first * 3 + i;
					if (E2E_[dedge] > dedge) {
						candidates.push_back(dedge);
					}
				}
			}
		}
		for (int k = prev_face_num; k < num_F_; ++k) {
			auto f = out_F_.row(k);
			for (int i = 0; i < 3; ++i) {
				int v0 = f[i];
				int v1 = f[(i + 1) % 3];
				if (v0 >= prev_vertex_num || v1 >= prev_vertex_num) {
					int dedge = k * 3 + i;
					if (E2E_[dedge] > dedge) {
						candidates.push_back(dedge);
					}
				}
			}
		}

		num_active_ = 0;
		for (int i = prev_vertex_num; i < num_V_; ++i) {
			active_vertices_[num_active_++] = i;
		}
		IterativeOptimize(len, true);
	}
}

void MeshProjector::OptimizePosition(int v, const Vector3& p, FT len, bool debug) {
	std::vector<Vector3> A;
	std::vector<FT> B;
	int deid = V2E_[v];
	int deid0 = deid;

	do {
		int v0 = out_F_(deid / 3, deid % 3);
		int v1 = out_F_(deid / 3, (deid + 1) % 3);
		int v2 = out_F_(deid / 3, (deid + 2) % 3);

		Vector3 vn[] = {out_N_.row(v0), out_N_.row(v1), out_N_.row(v2)};
		for (int i = 0; i < 3; ++i) {
			Vector3 d = (out_V_.row(v2) - out_V_.row(v1));
			d = d.cross(vn[i]).normalized();
			FT b = d.dot(out_V_.row(v1) - out_V_.row(v0));
			
			//b -= len * 0.05;

			A.push_back(d);
			B.push_back(b);

		}
		deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
	} while (deid0 != deid);

	std::vector<int> attached_dimensions(A.size(), 0);
	std::vector<Vector3> constraints;
	constraints.reserve(3);
	
	for (int i = 0; i < A.size(); ++i) {
		Vector3 offset = p - Vector3(out_V_.row(v));

		FT tar_step = offset.norm();
		if (tar_step < ZERO_THRES)
			return;

		Vector3 tar_dir = offset / tar_step;

#ifdef PROJ_THREE_TIMES
		if (constraints.size() == 1) {
			tar_dir = tar_dir - tar_dir.dot(constraints[0]) * constraints[0];
			FT n = tar_dir.norm();
			if (n < ZERO_THRES)
				return;
			tar_step *= n;
			tar_dir /= n;
		} 
		else if (constraints.size() == 2) {
			Vector3 dir = constraints[0].cross(constraints[1]).normalized();
			tar_dir = tar_dir.dot(dir) * dir;
			FT n = tar_dir.norm();
			if (n < ZERO_THRES)
				return;
			tar_step *= n;
			tar_dir /= n;			
		}
		else if (constraints.size() == 3) {
			return;
		}
#else
		if (constraints.size() > 0) {
			Vector3 c = constraints.back();
			Vector3 temp_dir = tar_dir - tar_dir.dot(c) * c;
			FT n = temp_dir.norm();
			if (n < ZERO_THRES)
				return;
			temp_dir /= n;
			int boundary_constraint = 0;
			Vector3 temp_boundary[3];
			for (int j = 0; j < constraints.size(); ++j) {
				FT denominator = constraints[j].dot(temp_dir);
				if (denominator > -1e-3) {
					temp_boundary[boundary_constraint] = constraints[j];
					boundary_constraint += 1;
				}
			}
			if (boundary_constraint == 3) {
				return;
			}
			if (boundary_constraint == 2) {
				temp_dir = temp_boundary[0].cross(temp_boundary[1]);
				if (temp_dir.dot(tar_dir) < 0)
					temp_dir = -temp_dir;
				FT n = temp_dir.norm();
				if (n < ZERO_THRES)
					return;
				temp_dir /= n;
				boundary_constraint = 0;
				for (int j = 0; j < constraints.size(); ++j) {
					FT denominator = constraints[j].dot(temp_dir);
					if (denominator > -1e-3)
						boundary_constraint += 1;
				}
				if (boundary_constraint == 3)
					return;
			}

			int top = 0;
			for (int j = 0; j < constraints.size(); ++j) {
				FT denominator = constraints[j].dot(temp_dir);
				if (denominator > -1e-3) {
					constraints[top++] = constraints[j];
				}
			}
			constraints.resize(top);
			if (top == 3) {
				return;
			}
			if (top == 2) {
				Vector3 c1 = constraints[0];
				Vector3 c2 = constraints[1];
				Vector3 dir = c1.cross(c2).normalized();
				temp_dir = tar_dir.dot(dir) * dir;
				n = temp_dir.norm();
				if (n < ZERO_THRES)
					return;
				temp_dir /= n;
			}
			tar_step *= n;
			tar_dir = temp_dir;
		}
#endif

		FT max_step = tar_step;
		if (debug)
			printf("Max step before %lf %lf\n", max_step);
		int constraint_id = -1;
		for (int j = 0; j < A.size(); ++j) {
			if (attached_dimensions[j])
				continue;
			FT denominator = A[j].dot(tar_dir);
			if (denominator < ZERO_THRES)
				continue;
			FT step = B[j] / denominator;
			if (step < max_step) {
				constraint_id = j;
				max_step = step;
			}
		}
		if (debug)
			printf("Max step after %lf\n", max_step);

		if (debug) {
			printf("Target dir <%f %f %f>\n", tar_dir[0], tar_dir[1], tar_dir[2]);
		}

		if (max_step < 1e-6)
			max_step = 0;

		out_V_.row(v) += max_step * tar_dir;

		if (max_step == tar_step)
			return;

		int constraint_size = constraints.size();
		int new_element = 0;
		for (int j = 0; j < A.size(); ++j) {
			if (attached_dimensions[j])
				continue;
			FT denominator = A[j].dot(tar_dir);
			B[j] -= denominator * max_step;

			if (B[j] < ZERO_THRES && denominator >= ZERO_THRES) {
				bool linear_dependent = false;
				if (constraint_size == 1
					&& constraints[0].cross(A[j]).norm() < ZERO_THRES)
					linear_dependent = true;
				if (constraint_size == 2) {
					Vector3 n = constraints[0].cross(constraints[1]);
					if (std::abs(n.normalized().dot(A[j])) < ZERO_THRES) {
						linear_dependent = true;
					}
				} 
				if (!linear_dependent) {
					if (new_element == 0) {
						constraints.push_back(A[j]);
						new_element = 1;
						attached_dimensions[j] = 1;
					}
				} else {
					attached_dimensions[j] = 1;
				}
			}
		}
	}
}

void MeshProjector::OptimizeNormal(int i, const Vector3& vn,
	const Vector3& target_vn) {
	Vector3 d = target_vn - vn;
	FT max_step = 1.0;
	int deid = V2E_[i];
	int deid0 = deid;
	do {
		Vector3 fn = out_FN_.row(deid / 3);
		FT denominator = d.dot(fn);
		if (denominator < -ZERO_THRES) {
			FT step = -fn.dot(vn) / denominator;
			if (step < max_step) {
				max_step = step;
			}
		}
		deid = E2E_[deid / 3 * 3 + (deid + 2) % 3];
	} while (deid0 != deid);
	if (max_step < 0) {
		max_step = 0;
	}
	out_N_.row(i) = vn + max_step * d;
}

void MeshProjector::OptimizeNormals() {
	MatrixD prev_norm = out_N_;
	UpdateVertexNormals(0);
	igl::per_face_normals(out_V_, out_F_, out_FN_);
	for (int i = 0; i < num_V_; ++i) {
		Vector3 vn = prev_norm.row(i);
		Vector3 target_vn = out_N_.row(i);
		OptimizeNormal(i, vn, target_vn);
	}
}

void MeshProjector::PreserveSharpFeatures(FT len_thres) {
	/*
	UpdateNearestDistance();
	MatrixD origin_FN;
	igl::per_face_normals(V_, F_, origin_FN);
	igl::per_face_normals(out_V_, out_F_, out_FN_);
	auto consistent = [&](int src_f0, int src_f1) {
		if (src_f0 == src_f1)
			return true;
		Vector3 n1 = origin_FN.row(src_f0);
		Vector3 n2 = origin_FN.row(src_f1);		
		FT norm_angle = std::abs(n1.dot(n2));
		if (norm_angle < std::cos(30 / 180.0 * 3.141592654)) {
			return false;
		}
		return true;
	};

	std::vector<int> sharp_edges;
	std::vector<Vector3> vertex_positions;
	for (int i = 0; i < num_F_; ++i) {
		for (int j = 0; j < 3; ++j) {
			int v0 = out_F_(i, j);
			int v1 = out_F_(i, (j + 1) % 3);
			if (v0 > v1)
				continue;
			int src_f0 = I_[v0];
			int src_f1 = I_[v1];
			if (consistent(src_f0, src_f1))
				continue;
			Vector3 p0 = V_.row(F_(src_f0, 0));
			Vector3 n0 = origin_FN.row(src_f0);

			Vector3 p1 = V_.row(F_(src_f1, 0));
			Vector3 n1 = origin_FN.row(src_f1);

			Vector3 o, t;
			if (!PlaneIntersect(p0, n0, p1, n1, &o, &t)) {
				continue;
			}

			Vector3 u1 = out_V_.row(v0);
			Vector3 u2 = out_V_.row(v1);
			Vector3 ntarget1 = (u1 - o).dot(t) * t + o;
			Vector3 ntarget2 = (u2 - o).dot(t) * t + o;

			if ((u1 - ntarget1).norm() > sqrt(3.0) * len_thres)
				continue;
			if ((u2 - ntarget2).norm() > sqrt(3.0) * len_thres)
				continue;
			sharp_edges.push_back(i * 3 + j);
			vertex_positions.push_back(ntarget1);
			vertex_positions.push_back(ntarget2);
		}
	}


	MatrixD P(vertex_positions.size(), 3);
	memcpy(P.data(), vertex_positions.data(),
		sizeof(Vector3) * vertex_positions.size());
	VectorX sqrD;
	VectorXi I;
	MatrixD tarP;
	*/
	UpdateNearestDistance();
	MatrixD origin_FN;
	igl::per_face_normals(V_, F_, origin_FN);
	auto consistent = [&](int src_f0, int src_f1) {
		if (src_f0 == src_f1)
			return true;
		Vector3 n1 = origin_FN.row(src_f0);
		Vector3 n2 = origin_FN.row(src_f1);		
		FT norm_angle = std::abs(n1.dot(n2));
		if (norm_angle < std::cos(60 / 180.0 * 3.141592654)) {
			return false;
		}
		return true;
	};

	std::vector<std::set<std::pair<int, int> > > vfeatures(num_V_);

	for (int i = 0; i < num_F_; ++i) {
		for (int j = 0; j < 3; ++j) {
			int v0 = out_F_(i, j);
			int v1 = out_F_(i, (j + 1) % 3);
			if (v1 < v0)
				continue;
			int src_f0 = I_[v0];
			int src_f1 = I_[v1];
			if (consistent(src_f0, src_f1))
				continue;
			if (src_f0 > src_f1)
				std::swap(src_f0, src_f1);
			auto key = std::make_pair(src_f0, src_f1);
			vfeatures[v0].insert(key);
			vfeatures[v1].insert(key);
		}
	}

	std::vector<std::pair<int, int> > face_elements;
	int vid = -1;
	std::vector<int> vertex_to_update;
	std::vector<Vector3> vertex_update_position;

	for (auto& v : vfeatures) {
		vid += 1;
		if (v.size() > 0) {
			face_elements.clear();
			for (auto& e : v)
				face_elements.push_back(e);

			Vector3 p0 = V_.row(F_(face_elements[0].first, 0));
			Vector3 n0 = origin_FN.row(face_elements[0].first);

			Vector3 p1 = V_.row(F_(face_elements[0].second, 0));
			Vector3 n1 = origin_FN.row(face_elements[0].second);

			Vector3 o, t;
			if (!PlaneIntersect(p0, n0, p1, n1, &o, &t)) {
				continue;
			}
			Vector3 target = out_V_.row(vid), ntarget;
			bool solved = false;

			if (face_elements.size() > 1) {
				std::set<int> fset;
				fset.insert(face_elements[0].first);
				fset.insert(face_elements[0].second);
				fset.insert(face_elements[1].first);
				fset.insert(face_elements[1].second);
				fset.erase(face_elements[0].first);
				fset.erase(face_elements[1].second);
				if (fset.size() > 1) {
					printf("...\n");
				}
				FT max_len = 1e30;
				for (auto& p : fset) {
					Vector3 p2 = V_.row(F_(p, 0));
					Vector3 n2 = origin_FN.row(p);
					if (std::abs(t.dot(n2)) > 0.1) {
						FT lambda = (p2 - o).dot(n2) / t.dot(n2);
						Vector3 nt = o + lambda * t;
						FT len = (nt - target).norm();
						if (len < max_len) {
							max_len = len;
							ntarget = nt;
						}
						solved = true;
					}
				}
			}
			if (!solved) {
				ntarget = (Vector3(target) - o).dot(t) * t + o;
			}
			if ((ntarget - target).norm() < len_thres || true) {
				vertex_to_update.push_back((solved) ? -(vid + 1) : vid + 1);
				vertex_update_position.push_back(ntarget);
			}
		}
	}
	MatrixD P(vertex_update_position.size(), 3);
	memcpy(P.data(), vertex_update_position.data(),
		sizeof(Vector3) * vertex_update_position.size());
	VectorX sqrD;
	VectorXi I;
	MatrixD tarP;
	sharp_vertices_.resize(num_V_, 0);
	sharp_positions_.resize(num_V_);
	igl::point_mesh_squared_distance(P, V_, F_, sqrD, I, tarP);

	for (int i = 0; i < sqrD.size(); i++) {
		if (sqrt(sqrD[i]) < 3e-2 * len_thres) {
			int v = vertex_to_update[i];
			int id = 1;
			if (v < 0) {
				id = 2;
				v = -v;
			}
			v -= 1;
			sharp_vertices_[v] = id;
			sharp_positions_[v] = vertex_update_position[i];
		}
	}

	/*
	std::ofstream os("../examples/sharps.obj");
	sharp_vertices_.resize(num_V_, 0);
	sharp_positions_.resize(num_V_);
	igl::point_mesh_squared_distance(P, V_, F_, sqrD, I, tarP);
	for (int i = 0; i < sqrD.size(); i += 2) {
		int dedge = sharp_edges[i / 2];
		int v0 = out_F_(dedge / 3, dedge % 3);
		int v1 = out_F_(dedge / 3, (dedge + 1) % 3);
		if (sqrt(sqrD[i]) < len_thres * 1e-1) {
			sharp_vertices_[v0] = 1;
			sharp_positions_[v0] = vertex_positions[i];
			os << "v " << vertex_positions[i][0] << " " << vertex_positions[i][1] << " " << vertex_positions[i][2] << "\n";
		}
		if (sqrt(sqrD[i + 1]) < len_thres * 1e-1) {
			sharp_vertices_[v1] = 1;
			sharp_positions_[v1] = vertex_positions[i + 1];
			os << "v " << vertex_positions[i+1][0] << " " << vertex_positions[i+1][1] << " " << vertex_positions[i+1][2] << "\n";
		}
	}
	os.close();
	*/
}