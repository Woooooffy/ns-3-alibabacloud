#ifndef COLL_HELPER_H
#define COLL_HELPER_H
#include "ns3/applications-module.h"
#include "ns3/collectives.h"
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
		protected:
			template<typename T> Ptr<Application> DoInstall(Ptr<Node> node){
				Ptr<CollectivesApplication> app = DynamicCast<CollectivesApplication, Application>(ApplicationHelper::DoInstall(node));
				app->SetAlgo(DynamicCast<T, Node>(node)->GetAlgo());
				return app;
			}
	};
}
#endif
