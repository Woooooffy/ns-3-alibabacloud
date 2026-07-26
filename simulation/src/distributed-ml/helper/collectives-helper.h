#ifndef COLL_HELPER_H
#define COLL_HELPER_H
#include "ns3/applications-module.h"
#include "ns3/collectives.h"
#include "ns3/algo_topology.h"
namespace ns3 {

	class CollectivesApplicationHelper : public ApplicationHelper {

		public:
			CollectivesApplicationHelper();
			template<typename T> ApplicationContainer Install(NodeContainer c){
				ApplicationContainer apps;
				for (auto i = c.Begin(); i != c.End(); ++i){
						apps.Add(DoInstall<T>(*i));
				}
				return apps;
			}
			template<typename T> ApplicationContainer Install(Ptr<Node> node){
				return ApplicationContainer(DoInstall<T>(node));
			}
			// Install on exactly the active GPUs of a parsed AlgoTopology (those carrying a
			// non-empty algorithm), in ascending gpu-id order. The returned container's app
			// index therefore equals the participant rank the algorithm XML assumes, so it can
			// be handed straight to CollectiveTester with no passive-GPU bookkeeping.
			template<typename T> ApplicationContainer Install(AlgoTopology& topo){
				ApplicationContainer apps;
				for (int gpuId : topo.GetActiveGpuIds()){
					apps.Add(DoInstall<T>(topo.GetGpuNode(gpuId)));
				}
				return apps;
			}
		protected:
			template<typename T> Ptr<Application> DoInstall(Ptr<Node> node){
				Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication, Application>(ApplicationHelper::DoInstall(node));
				app->SetAlgo(DynamicCast<T, Node>(node)->GetAlgo());
				return app;
			}
	};
}
#endif
