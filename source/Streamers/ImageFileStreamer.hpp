#ifndef META_IMAGE_STREAMER_HPP
#define META_IMAGE_STREAMER_HPP

#include "SmartPointers.hpp"
#include "Streamer.hpp"
#include "Image.hpp"
#include "ProcessObject.hpp"
#include <boost/thread.hpp>

namespace fast {

class ImageFileStreamer : public Streamer, public ProcessObject {
    FAST_OBJECT(ImageFileStreamer)
    public:
        void setFilenameFormat(std::string str);
        void setStartNumber(uint startNumber);
        void setZeroFilling(uint digits);
        void setMaximumNumberOfFrames(uint nrOfFrames);
        void enableLooping();
        void disableLooping();
        bool hasReachedEnd() const;
        uint getNrOfFrames() const;
        // This method runs in a separate thread and adds frames to the
        // output object
        void producerStream();

        ~ImageFileStreamer();
    private:
        ImageFileStreamer();

        // Update the streamer if any parameters have changed
        void execute();

        bool mLoop;
        uint mZeroFillDigits;
        uint mStartNumber;
        uint mNrOfFrames;
        uint mMaximumNrOfFrames;

        boost::thread *thread;
        boost::mutex mFirstFrameMutex;
        boost::condition_variable mFirstFrameCondition;

        bool mStreamIsStarted;
        bool mFirstFrameIsInserted;
        bool mHasReachedEnd;

        std::string mFilenameFormat;

};

} // end namespace fast

#endif
