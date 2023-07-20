#include "BoundedBuffer.h"
#include <iostream>
using namespace std;


BoundedBuffer::BoundedBuffer (int _cap) : cap(_cap) {
    // modify as needed
}

BoundedBuffer::~BoundedBuffer () {
    // modify as needed
}

void BoundedBuffer::push (char* msg, int size) {
    // 1. Convert the incoming byte sequence given by msg and size into a vector<char>
    //      use one of the vector constructors 
     vector<char> item(msg, msg + size);
    // 2. Wait until there is room in the queue (i.e., queue lengh is less than cap)
    //      waiting on slot available
    unique_lock<mutex> lock(mut);
    while (q.size() >= (long unsigned int)cap) {
        slot_av.wait(lock);
    }
    // 3. Then push the vector at the end of the queue
    q.push(item);
    // 4. Wake up threads that were waiting for push
    //      notifying data available
    data_av.notify_one();

    cout << "q push size:" << q.size() << endl;
}

int BoundedBuffer::pop (char* msg, int size) {
    // 1. Wait until the queue has at least 1 item
    //         waiting on data available
    unique_lock<mutex> lock(mut);
    while (q.empty()) {
        data_av.wait(lock);
    }
    // 2. Pop the front item of the queue. The popped item is a vector<char>
    vector<char> item = q.front();
    q.pop();
    cout << "q pop size:" << q.size() << endl;
    // 3. Convert the popped vector<char> into a char*, copy that into msg; assert that the vector<char>'s length is <= size
    // use vector::data()
    assert(item.size() <= (long unsigned int)size);
    memcpy(msg, item.data(), item.size());
    // 4. Wake up threads that were waiting for pop
    //      notifying slot available
    slot_av.notify_all();
    // 5. Return the vector's length to the caller so that they know how many bytes were popped
    return item.size();
}

size_t BoundedBuffer::size () {
    return q.size();
}
