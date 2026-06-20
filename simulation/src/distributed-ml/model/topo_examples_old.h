#ifndef TOPO_EXAMPLE_H
#define TOPO_EXAMPLE_H

namespace ns3{
	template <int N> class LinkSet {
		public:
			LinkSet(){}
			LinkSet(const std::array<std::pair<int,int>, N>& data): m_edgeArray(data){}
			~LinkSet(){}
			std::array<std::pair<int, int>, N> GetEdgeArray(){
				return m_edgeArray;
			}
		protected:
			std::array<std::pair<int, int>, N> m_edgeArray;
	};

	template <int N> class Line : public LinkSet<(N - 1) * 2>{
		public:
			Line(){
				for (int i = 0; i < N - 1; ++i){
					this->m_edgeArray[i] = {i, i + 1};
				}
				for (int i = 1; i < N; ++i){
					this->m_edgeArray[N - 2 + i] = {i, i - 1};
				}
			}
			~Line(){}
	};

	template <int N> class Ring : public LinkSet<N * 2>{
		public:
			Ring(){
				for (int i = 0; i < N; ++i){
					this->m_edgeArray[i] = {i, (i + 1) % N};
					this->m_edgeArray[N + i] = {i, (i + N - 1) % N};
				}
			}
			~Ring(){}
	};

	template <int N> class FullyConnected: public LinkSet<N * (N - 1)>{
		public:
			FullyConnected(){
				int ind = 0;
				for (int i = 0; i < N; ++i){
					for (int j = 0; j < N; ++j){
						if (i == j) continue;
						this->m_edgeArray[ind] = {i, j};
						++ind;
					}
				}
			}
			~FullyConnected(){}
	};

	inline LinkSet<24> DGX1(
		{{
    {0, 1}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 4},
    {1, 2}, {1, 3}, {1, 3}, {2, 3}, {2, 3}, {2, 6},
    {2, 6}, {3, 7}, {4, 5}, {4, 5}, {4, 6}, {4, 7},
    {5, 6}, {5, 7}, {5, 7}, {6, 7}, {6, 7}, {1, 5}
		}});
} // namespace ns3


#endif
