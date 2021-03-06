#ifndef MATTERN_GVT_MANAGER_HPP
#define MATTERN_GVT_MANAGER_HPP

#include <memory> // for unique_ptr

#include "TimeWarpEventDispatcher.hpp"
#include "serialization.hpp"
#include "TimeWarpKernelMessage.hpp"

/* This class implements the Mattern algorithm, which is a distributed token passing algorithm used
 * to estimate the GVT. Each node contains a single object of this class.
 *
 * Each node is either red or white, all initially white.
 * Event messages which contain timestamps are colored when they are sent. White nodes will send
 * white messages and red nodes will send red messages.
 *
 * Each node must keep track of:
 *  1. It's color
 *  2. Accumulated minumum timestamp of red messages it has received
 *      (after red message has been received)
 *  3. White messages it has sent minus white messages it has received (NOTE: This is one version
 *      of the algorithm. Vector counters could be used instead to keep track of any outstanding
 *      white messages at all nodes.)
 *
 * A mattern token consists of:
 *  1. Accumulated value of the minimum of all local simulation times (lvt values)
 *  2. Accumulated minimum of the red message timestamps for all nodes that have received token
 *  3. Accumulated count of all white messages in transit (This could also be a vector count
 *      consisting of outstanding messages at all nodes)
 *
 * Description of implemented algorithm:
 *  1. The designated node (mpi_rank == 0) starts the gvt estimation by sending an initial token
 *       with its min lvt, min red message timestamp of infinity and its white msg counter.
 *  2. When another node receives a token, if it is already red then the min red msg timestamp
 *      is updated. If it is white, then it becomes red.
 *  3. When the initiator node (mpi_rank = 0) receives a the toke back and the accumulated white
 *      msg count is zero, then the gvt is estimated by taking the minimum of the min red msg
 *      timestamps and the minimum of all local simulation times. If the accumulated white message
 *      count is not zero, another round of tokens is inititated.
 *  NOTE: In this implemenation the token is passed in logical ring fashion using the mpi rank as
 *      an id.
 */

namespace warped {

enum class MatternColor { WHITE, RED };

class TimeWarpMatternGVTManager {
public:
    TimeWarpMatternGVTManager(std::shared_ptr<TimeWarpCommunicationManager> comm_manager,
        unsigned int period) :
        comm_manager_(comm_manager), gvt_period_(period) {}

    void initialize();

    bool startGVT();

    void resetState();

    void receiveEventUpdateState(MatternColor color);

    MatternColor sendEventUpdateState(unsigned int timestamp);

    void sendMatternGVTToken(unsigned int local_minimum);

    void receiveMatternGVTToken(std::unique_ptr<TimeWarpKernelMessage> msg);

    void receiveGVTUpdate(std::unique_ptr<TimeWarpKernelMessage> kmsg);

    unsigned int getGVT() { return gVT_; }

    void setGVT(unsigned int gvt) { gVT_ = gvt; }

    bool needLocalGVT();

    bool gvtUpdated();

protected:
    unsigned int infinityVT();

    void sendGVTUpdate(unsigned int gvt);

private:
    unsigned int gVT_ = 0;

    unsigned int token_iteration_ = 0;

    MatternColor color_ = MatternColor::WHITE;
    int white_msg_counter_ = 0;

    unsigned int min_red_msg_timestamp_ = infinityVT();
    unsigned int global_min_;
    int msg_count_ = 0;

    bool gVT_token_pending_ = false;

    std::chrono::time_point<std::chrono::steady_clock> gvt_start;

    const std::shared_ptr<TimeWarpCommunicationManager> comm_manager_;

    unsigned int gvt_period_;

    bool need_local_gvt_ = false;

    bool gvt_updated_ = false;

};

struct MatternGVTToken : public TimeWarpKernelMessage {
    MatternGVTToken() = default;
    MatternGVTToken(unsigned int sender, unsigned int receiver, unsigned int mc, unsigned int ms,
        unsigned int c) :
        TimeWarpKernelMessage(sender, receiver),
        m_clock(mc),
        m_send(ms),
        count(c) {}

    // Accumulated minimum of all local simulation clocks (lvt values)
    unsigned int m_clock;

    // Accumulated minimum of all red message timestamps
    unsigned int m_send;

    // Accumulated total of white messages in transit
    int count;

    MessageType get_type() { return MessageType::MatternGVTToken; }

    WARPED_REGISTER_SERIALIZABLE_MEMBERS(cereal::base_class<TimeWarpKernelMessage>(this), m_clock,
                                         m_send, count)

};

struct GVTUpdateMessage : public TimeWarpKernelMessage {
    GVTUpdateMessage() = default;
    GVTUpdateMessage(unsigned int sender_id, unsigned int receiver_id, unsigned int gvt) :
        TimeWarpKernelMessage(sender_id, receiver_id), new_gvt(gvt) {}

    unsigned int new_gvt;

    MessageType get_type() { return MessageType::GVTUpdateMessage; }

    WARPED_REGISTER_SERIALIZABLE_MEMBERS(cereal::base_class<TimeWarpKernelMessage>(this), new_gvt)
};

} // warped namespace

#endif
