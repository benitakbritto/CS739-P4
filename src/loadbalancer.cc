#include <iostream>
#include <memory>
#include <string>
#include <grpcpp/grpcpp.h>
#include "keyvalueops.grpc.pb.h"
#include "lb.grpc.pb.h"
#include "client.h"
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include "util/common.h"

/******************************************************************************
 * NAMESPACES
 *****************************************************************************/
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::ServerReaderWriter; 
using namespace kvstore;
using namespace std;

/******************************************************************************
 * MACROS
 *****************************************************************************/

/******************************************************************************
 * GLOBALS
 *****************************************************************************/
unordered_map<string, pair<int, KeyValueClient*>> nodes; // <ip: <id, stub>>
string leaderIP;

/******************************************************************************
 * DECLARATION
 *****************************************************************************/
/*
*   @brief Replays client requests to the server node(s)
*/
class KeyValueService final : public KeyValueOps::Service 
{
private:
    int idx = 0; // for round-robin

    /*
    *   @brief Gets server ip using round robin
    *
    *   @return Server stub
    */
    KeyValueClient* getServerIPToRouteTo()
    {
        idx = (++idx) % (nodes.size());
        auto it = nodes.begin();
        advance(it, idx);
        dbgprintf("[DEBUG] %s: Routing to %s\n", __func__, (it->first).c_str());
        return it->second.second;
    }

    /*
    *   @brief receive client request from client
    *                 and replay to server node(s)
    *
    *   @param context 
    *   @param request 
    *   @param response 
    *   @param offset 
    *   @return gRPC status
    */
    Status GetFromDB(ServerContext* context, const GetRequest* request, GetReply* reply) override 
    {
        dbgprintf("[DEBUG] %s: Entering function\n", __func__);
        KeyValueClient* stub = getServerIPToRouteTo();
        return stub->GetFromDB(*request, reply);
    }

    Status PutToDB(ServerContext* context,const PutRequest* request, PutReply* reply) override 
    {
        dbgprintf("[DEBUG] %s: Entering function\n", __func__);
        dbgprintf("LeaderIP = %s\n", leaderIP.c_str());
        
        KeyValueClient* stub = nodes[leaderIP].second;
        
        return stub->PutToDB(*request, reply);
    } 

public:
    KeyValueService(){}
};

/******************************************************************************
 * DECLARATION
 *****************************************************************************/
/*
*   @brief Communication between the LB and the server nodes (via heartbeats)
*/
class LBNodeCommService final: public LBNodeComm::Service 
{
private:

    /*
    *   @brief Save server node information
    *
    *   @param identity  
    *   @param ip 
    */
    void registerNode(int identity, string ip) 
    {
        nodes[ip] = make_pair(identity,
                    new KeyValueClient (grpc::CreateChannel(ip, grpc::InsecureChannelCredentials())));
        
        dbgprintf("[DEBUG]: Length of nodes at LB: %ld", nodes.size());
    }

    /*
    *   @brief Remove node information from in-mem
    *
    *   @param ip 
    */
    void eraseNode(string ip) 
    {
        nodes.erase(ip);
        dbgprintf("[DEBUG] %s: Removed ip %s, Length of nodes at LB: %ld\n", __func__, ip.c_str(), nodes.size());
    }

    /*
    *   @brief Change identity of ip to Leader
    *
    *   @param ip 
    */
    void updateLeader(string ip) 
    {
        if(!leaderIP.empty()) 
        {
            nodes[leaderIP].first = FOLLOWER;
        }
        leaderIP = ip;
        nodes[leaderIP].first = LEADER;
    }

    /*
    *   @brief Helper function
    *
    *   @param reply 
    */
    void addNodeDataToReply(HeartBeatReply* reply) 
    {
        NodeData* nodeData;
        for (auto& it: nodes) 
        {
            nodeData = reply->add_node_data();
            nodeData->set_ip(it.first);
            nodeData->set_identity(it.second.first);
        }
    }

public:
    LBNodeCommService() {}

    /*
    *   @brief Receives heartbeat from a server node, 
    *          registers node if LB hasn't seen the node before
    *          and sends back a list of all the alive server nodes in the reply
    *
    *   @param context
    *   @param stream 
    */
    Status SendHeartBeat(ServerContext* context, ServerReaderWriter<HeartBeatReply, HeartBeatRequest>* stream) override 
    {
        HeartBeatRequest request;
        HeartBeatReply reply;

        int identity;
        string ip;
        bool registerFirstTime = true;

        while(1) 
        {
            if(!stream->Read(&request)) 
            {
                break;
            }
            dbgprintf("[INFO] %s: recv heartbeat from IP:[%s]\n", __func__, request.ip().c_str());

            identity = request.identity();
            ip = request.ip();

            if(registerFirstTime) 
            {
                dbgprintf("[DEBUG] %s: Registering node %s for the 1st time\n", __func__, ip.c_str());
                registerNode(identity, ip);
            }

            registerFirstTime = false;
                
            if(identity == LEADER)
            {
                updateLeader(ip);
            }

            reply.Clear();
            addNodeDataToReply(&reply);
                
            if(!stream->Write(reply)) 
            {
                break;
            }
            dbgprintf("[INFO] %s: sent heartbeat reply\n", __func__);
        }

        cout << "[ERROR]: stream broke" << endl;
        eraseNode(ip);

        return Status::OK;
    }
};

/*
*   @brief Starts a service for client requests 
*/
void* RunServerForClient(void* arg) 
{
    string server_address("0.0.0.0:50051"); // TODO: set from cli
    KeyValueService service;

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    unique_ptr<Server> server(builder.BuildAndStart());
    cout << "LB Server for Client listening on " << server_address << std::endl;

    server->Wait();

    return NULL;
}

/*
*   @brief Starts a service for server node heartbeat requests 
*/
void* RunServerForNodes(void* arg) 
{
    string server_address("0.0.0.0:50052");
    LBNodeCommService service;
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    unique_ptr<Server> server(builder.BuildAndStart());
    cout << "LB Server for Nodes listening on " << server_address << std::endl;

    server->Wait();

    return NULL;
}

/******************************************************************************
 * DRIVER
 *****************************************************************************/
int main (int argc, char *argv[]){
    pthread_t client_server_t, node_server_t;
  
    pthread_create(&client_server_t, NULL, RunServerForClient, NULL);
    pthread_create(&node_server_t, NULL, RunServerForNodes, NULL);

    pthread_join(client_server_t, NULL);
    pthread_join(node_server_t, NULL);
}