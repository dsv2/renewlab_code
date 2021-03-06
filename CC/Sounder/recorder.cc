/*
 Copyright (c) 2018-2020, Rice University 
 RENEW OPEN SOURCE LICENSE: http://renew-wireless.org/license

----------------------------------------------------------------------
 Record received frames from massive-mimo base station in HDF5 format
---------------------------------------------------------------------
*/
#include "include/recorder.h"
#include "include/logger.h"
#include "include/macros.h"
#include "include/signalHandler.hpp"
#include "include/utils.h"

namespace Sounder {
// buffer length of each rx thread
const int Recorder::kSampleBufferFrameNum = 80;
// dequeue bulk size, used to reduce the overhead of dequeue in main thread
const int Recorder::KDequeueBulkSize = 5;

#if (DEBUG_PRINT)
const int kDsSim = 5;
#endif

static const int kQueueSize = 36;

Recorder::Recorder(Config* in_cfg, unsigned int core_start)
    : cfg_(in_cfg)
    , kMainDispatchCore(core_start)
    , kRecorderCore(kMainDispatchCore + 1)
    , kRecvCore(kRecorderCore + in_cfg->task_thread_num())
{
    size_t rx_thread_num = cfg_->rx_thread_num();
    size_t ant_per_rx_thread = cfg_->bs_present() && rx_thread_num > 0
        ? cfg_->getTotNumAntennas() / rx_thread_num
        : 1;
    rx_thread_buff_size_
        = kSampleBufferFrameNum * cfg_->symbols_per_frame() * ant_per_rx_thread;

    message_queue_ = moodycamel::ConcurrentQueue<Event_data>(
        rx_thread_buff_size_ * kQueueSize);

    MLPD_TRACE("Recorder construction: rx threads: %zu, recorder threads: %u, "
               "chunk size: %zu\n",
        rx_thread_num, cfg_->task_thread_num(), rx_thread_buff_size_);

    if (rx_thread_num > 0) {
        // initialize rx buffers
        rx_buffer_ = new SampleBuffer[rx_thread_num];
        size_t intsize = sizeof(std::atomic_int);
        size_t arraysize = (rx_thread_buff_size_ + intsize - 1) / intsize;
        size_t packageLength = sizeof(Package) + cfg_->getPackageDataLength();
        for (size_t i = 0; i < rx_thread_num; i++) {
            rx_buffer_[i].buffer.resize(rx_thread_buff_size_ * packageLength);
            rx_buffer_[i].pkg_buf_inuse = new std::atomic_int[arraysize];
            std::fill_n(rx_buffer_[i].pkg_buf_inuse, arraysize, 0);
        }
    }

    // Receiver object will be used for both BS and clients
    try {
        receiver_.reset(new Receiver(rx_thread_num, cfg_, &message_queue_));
    } catch (std::exception& e) {
        std::cout << e.what() << '\n';
        gc();
        throw runtime_error("Error Setting up the Receiver");
    }
}

void Recorder::gc(void)
{
    MLPD_TRACE("Garbage collect\n");
    this->receiver_.reset();
    if (this->cfg_->rx_thread_num() > 0) {
        for (size_t i = 0; i < this->cfg_->rx_thread_num(); i++) {
            delete[] this->rx_buffer_[i].pkg_buf_inuse;
        }
        delete[] this->rx_buffer_;
    }
}

Recorder::~Recorder() { this->gc(); }

void Recorder::do_it()
{
    size_t recorder_threads = this->cfg_->task_thread_num();
    size_t total_antennas = cfg_->getTotNumAntennas();
    size_t thread_antennas = 0;
    std::vector<pthread_t> recv_threads;

    MLPD_TRACE("Recorder work thread\n");
    if ((this->cfg_->core_alloc() == true)
        && (pin_to_core(kMainDispatchCore) != 0)) {
        MLPD_ERROR("Pinning main recorder thread to core 0 failed");
        throw std::runtime_error(
            "Pinning main recorder thread to core 0 failed");
    }

    if (this->cfg_->client_present() == true) {
        auto client_threads = this->receiver_->startClientThreads();
    }

    if (this->cfg_->rx_thread_num() > 0) {

        thread_antennas = (total_antennas / recorder_threads);
        // If antennas are left, distribute them over the threads. This may assign antennas that don't
        // exist to the threads at the end. This isn't a concern.
        if ((total_antennas % recorder_threads) != 0) {
            thread_antennas = (thread_antennas + 1);
        }

        for (unsigned int i = 0u; i < recorder_threads; i++) {
            int thread_core = -1;
            if (this->cfg_->core_alloc() == true) {
                thread_core = kRecorderCore + i;
            }

            MLPD_INFO("Creating recorder thread: %u, with antennas %zu:%zu "
                      "total %zu\n",
                i, (i * thread_antennas), ((i + 1) * thread_antennas) - 1,
                thread_antennas);
            Sounder::RecorderThread* new_recorder
                = new Sounder::RecorderThread(this->cfg_, i, thread_core,
                    (this->rx_thread_buff_size_ * kQueueSize),
                    (i * thread_antennas), thread_antennas, true);
            new_recorder->Start();
            this->recorders_.push_back(new_recorder);
        }

        // create socket buffer and socket threads
        recv_threads
            = this->receiver_->startRecvThreads(this->rx_buffer_, kRecvCore);
    } else
        this->receiver_->go(); // only beamsweeping

    moodycamel::ConsumerToken ctok(this->message_queue_);

    Event_data events_list[KDequeueBulkSize];
    int ret = 0;

    /* TODO : we can probably remove the dispatch function and pass directly to the recievers */
    while ((this->cfg_->running() == true)
        && (SignalHandler::gotExitSignal() == false)) {
        // get a bulk of events from the receivers
        ret = this->message_queue_.try_dequeue_bulk(
            ctok, events_list, KDequeueBulkSize);
        //if (ret > 0)
        //{
        //    MLPD_TRACE("Message(s) received: %d\n", ret );
        //}
        // handle each event
        for (int bulk_count = 0; bulk_count < ret; bulk_count++) {
            Event_data& event = events_list[bulk_count];

            // if kEventRxSymbol, dispatch to proper worker
            if (event.event_type == kEventRxSymbol) {
                size_t thread_index = event.ant_id / thread_antennas;
                int offset = event.data;
                Sounder::RecorderThread::RecordEventData do_record_task;
                do_record_task.event_type
                    = Sounder::RecorderThread::RecordEventType::kTaskRecord;
                do_record_task.data = offset;
                do_record_task.rx_buffer = this->rx_buffer_;
                do_record_task.rx_buff_size = this->rx_thread_buff_size_;
                // Pass the work off to the applicable worker
                // Worker must free the buffer, future work would involve making this cleaner

                //If no worker threads, it is possible to handle the event directly.
                //this->worker_.handleEvent(do_record_task, 0);
                if (this->recorders_.at(thread_index)
                        ->DispatchWork(do_record_task)
                    == false) {
                    MLPD_ERROR("Record task enqueue failed\n");
                    throw std::runtime_error("Record task enqueue failed");
                }
            }
        }
    }
    this->cfg_->running(false);
    this->receiver_->completeRecvThreads(recv_threads);
    this->receiver_.reset();

    /* Force the recorders to process all of the data they have left and exit cleanly
         * Send a stop to all the recorders to allow the finalization to be done in parrallel */
    for (auto recorder : this->recorders_) {
        recorder->Stop();
    }
    for (auto recorder : this->recorders_) {
        delete recorder;
    }
    this->recorders_.clear();
}

int Recorder::getRecordedFrameNum() { return this->max_frame_number_; }

extern "C" {
Recorder* Recorder_new(Config* in_cfg)
{
    Recorder* rec = new Recorder(in_cfg);
    return rec;
}

void Recorder_start(Recorder* rec) { rec->do_it(); }
int Recorder_getRecordedFrameNum(Recorder* rec)
{
    return rec->getRecordedFrameNum();
}
const char* Recorder_getTraceFileName(Recorder* rec)
{
    return rec->getTraceFileName().c_str();
}
}
}; // end namespace Sounder
