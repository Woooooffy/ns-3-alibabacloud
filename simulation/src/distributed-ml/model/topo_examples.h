#ifndef TOPO_EXAMPLE_H
#define TOPO_EXAMPLE_H

namespace ns3{
	template <int N> class AdjacencyMat {
		public:
			AdjacencyMat(){}
			AdjacencyMat(const std::array<std::array<int, N>, N>& data): m_adjacency(data){}
			~AdjacencyMat(){}
			std::array<std::array<int, N>, N> GetAdjacency(){
				return m_adjacency;
			}
		protected:
			std::array<std::array<int, N>, N> m_adjacency{};
	};

	template <int N> class Line : public AdjacencyMat<N>{
		public:
			Line(){
				for (int i = 0; i < N; ++i){
					if (i + 1 < N) this->m_adjacency[i][i + 1] = 1;
					if (i - 1 >= 0) this->m_adjacency[i][i - 1] = 1;
				}
			}
			~Line(){}
	};

	template <int N> class Ring : public AdjacencyMat<N>{
		public:
			Ring(){
				for (int i = 0; i < N; ++i){
					this->m_adjacency[i][(i + 1) % N] = 1;
					this->m_adjacency[i][(i + N - 1) % N] = 1;
				}
			}
			~Ring(){}
	};

	template <int N> class FullyConnected: public AdjacencyMat<N>{
		public:
			FullyConnected(){
				for (int i = 0; i < N; ++i){
					for (int j = 0; j < N; ++j){
						this->m_adjacency[i][j] = (i!=j);
					}
				}
			}
			~FullyConnected(){}
	};

	template <int N> class HubAndSpoke: public AdjacencyMat<N + 1>{
		public:
			HubAndSpoke(){
				for (int i = 0; i < N; ++i){
					// last node is the switch
					this->m_adjacency[N][i] = 1;
					this->m_adjacency[i][N] = 1;
				}
			}
	};

inline constexpr std::array<std::array<int,8>,8> DGX1_DATA{{
    {{0,2,1,1,2,0,0,0}},
    {{2,0,1,2,0,1,0,0}},
    {{1,1,0,2,0,0,2,0}},
    {{1,2,2,0,0,0,0,1}},
    {{2,0,0,0,0,2,1,1}},
    {{0,1,0,0,2,0,1,2}},
    {{0,0,2,0,1,1,0,2}},
    {{0,0,0,1,1,2,2,0}}
}};

inline AdjacencyMat<8> DGX1(DGX1_DATA);
} // namespace ns3


#endif
