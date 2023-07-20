#include <fstream>
#include <iostream>
#include <thread>
#include <sys/time.h>
#include <sys/wait.h>
#include <algorithm>
#include <sys/stat.h> 

#include "BoundedBuffer.h"
#include "common.h"
#include "Histogram.h"
#include "HistogramCollection.h"
#include "FIFORequestChannel.h"

// ecgno to use for datamsgs
#define EGCNO 1

using namespace std;

struct Response {
    int person;
    double value;

    Response(int p, double v) : person(p), value(v) {}
};

void patient_thread_function (int patient_no, int n, BoundedBuffer* request_buffer) {
    // functionality of the patient threads
    // take a paitent p_no
    //for n requests, produce a datamsg(p_no, time, ECGNO) and push to reqest_buffer
    //      -time dependent on currrent reuqests
    //      - at 0 -> time = 0.00; at 1 -> time = 0.004; at 2 -> time = 0.008;...

    for (int i = 0; i < n; i++) {
        double time = i * 0.004; // compute the time for the current request
        datamsg request = datamsg(patient_no, time, EGCNO); // create a datamsg object for the current request
        request_buffer->push((char*)&request, sizeof(request)); // push the request onto the buffer
        
    }

}

void file_thread_function (int bufsize, FIFORequestChannel* chan, string filename, BoundedBuffer* request_buffer) {
    // functionality of the file thread
    // get file size (do 00 appended with filename) 
    char buf1[sizeof(filemsg)+filename.size()+1];
    filemsg file(0,0);
    memcpy(buf1, &file, sizeof(filemsg));
    strcpy(buf1+sizeof(filemsg), filename.c_str());

    cout<<"Making Request"<<endl;
    chan->cwrite(buf1, sizeof(buf1));

    __int64_t filesize;
    chan->cread(&filesize, sizeof(__int64_t));
    cout<<"receiveing request"<<endl;
    //int filesize = get_file_size(filename);
    // open output file; allocate the memory of file with fseek; close the file
    
    // open output file
    string output_filename = "received/" + filename;
    FILE* outfile = fopen(output_filename.c_str(), "wb");
    if (outfile == NULL) {
        perror("Error opening output file");
        return;
    }

    // allocate the memory of file with fseek
    fseek(outfile, filesize, SEEK_SET); //TWC: Just to the filesize is fine
    fclose(outfile);
     
    filemsg* msg = (filemsg*) buf1;
    // while offset is less than file_size, 
    //      - produce a filemsg(offset, m(lengh of message)) + filename and push to request_biffer
    //      - icrementing offset and be careful with the final message
    int offset = 0;
    while (offset < filesize) {
        int remaining = filesize - offset;
        int len = min(remaining, bufsize);
        msg->offset = offset;
        msg->length = len;
        //cout<<"PUSHED"<<endl;
        request_buffer->push(buf1, sizeof(buf1));
        offset += len;

    }
    //cout<<"File Thread Function Exited"<<endl;

}

void worker_thread_function ( /*ADD ARGUMENTS*/ int bufsize, BoundedBuffer* request_buffer, BoundedBuffer* response_buffer, FIFORequestChannel* fifo) {
    // functionality of the worker threads
    //forever loop
    // pop message from the request_buffer
    // view line 120 in server(process_request function) for how to decide current messagge
    // if DATA:
    //      ssend the request across a FIFO channel
    //      collect repsonse
    //      create a Resposne(struct?) of p_no from message and repsonse from server
    //      push that Resposne to the response_buffer
    // IF FILE:
    //      collect the file name from the message
    //      open file in update mode, 
    //      fseek(SEEK_SET) to offset of the filemsg
    //      write the buffer from the server
 while(true){
    char msg[bufsize];
    int len = request_buffer->pop(msg, bufsize);
    MESSAGE_TYPE mtype = (*(MESSAGE_TYPE*)msg);
    cout << "MESSAGE: " << mtype << endl;
    if( mtype == DATA_MSG){
        
        fifo->cwrite(msg, sizeof(datamsg));
        // Receive response from server
        double response;
        fifo->cread(&response, sizeof(double));
      //  cout << "fifo recv. repsonse: " << response << endl;
        // Extract person number from message
        datamsg* d = (datamsg*) msg;
        int person = d->person;

        // Create and push response to response buffer
        Response resp = Response(person, response);
        //print response
        response_buffer->push((char*)&resp, sizeof(Response));

    }
    else if(mtype == FILE_MSG){
        
        cout <<"Filemsg received"<<endl;
        filemsg* fmsg = (filemsg*)msg;
        string filename = (char*) (fmsg+1);
        cout<<"FileName asff"<<filename<<endl;
        FILE* fp = fopen(("received/"+filename).c_str(), "rb+");
        if (!fp) {
            perror("Error opening file");
            return;
        }
        fseek(fp, fmsg->offset, SEEK_SET);
        
        fifo->cwrite(&msg,  (sizeof(filemsg) + filename.size() + 1));
        char buffer[bufsize];
        int nbytes = fifo->cread(buffer, fmsg->length);

        int bytes_written = fwrite(buffer, sizeof(char), fmsg->length, fp);
        if (bytes_written < nbytes) {
            perror("Error writing to file");
            return;
        }

        fclose(fp);
    }
    else if(mtype == QUIT_MSG){
        //push special Response
        // Response quit_resp = Response(-1, -1);
        // response_buffer->push((char*)&quit_resp, sizeof(Response));
        fifo->cwrite(msg, sizeof(MESSAGE_TYPE));
        break;
    }
    }
}


void histogram_thread_function (BoundedBuffer* response_buffer, HistogramCollection* hc) {
    // functionality of the histogram threads

    //forver loop
    // pop response from the repsonse_buffer
    // call hisogram::update(resp->p_no, resp->double)
    while (true) {
        // Pop response from the response buffer
        char buffer[sizeof(Response)];
        response_buffer->pop(buffer, sizeof(Response));

        // Check if done    
        Response resp(0,0);
        memcpy(&resp, buffer, sizeof(Response));
        if (resp.person == -1 && resp.value == -1) {
            break;
        }
        
        // Update histogram with the response value
        hc->update(resp.person, resp.value);
    }
   }



int main (int argc, char* argv[]) {
    int n = 1000;	// default number of requests per "patient"
    int p = 10;		// number of patients [1,15]
    int w = 100;	// default number of worker threads
	int h = 20;		// default number of histogram threads
    int b = 20;		// default capacity of the request buffer (should be changed)
	int m = MAX_MESSAGE;	// default capacity of the message buffer
	string f = "";	// name of file to be transferred
    
    // read arguments
    int opt;
	while ((opt = getopt(argc, argv, "n:p:w:h:b:m:f:")) != -1) {
		switch (opt) {
			case 'n':
				n = atoi(optarg);
                break;
			case 'p':
				p = atoi(optarg);
                break;
			case 'w':
				w = atoi(optarg);
                break;
			case 'h':
				h = atoi(optarg);
				break;
			case 'b':
				b = atoi(optarg);
                // if(b < 10){ b = 10;}
                // if(b > 200){b = 200;}
                break;
			case 'm': 
				m = atoi(optarg);
                break;
			case 'f':
				f = optarg;
                break;
		}
	}
    cout << "This is b: " << b << endl;
    
	// fork and exec the server
    int pid = fork();
    if (pid == 0) {
        execl("./server", "./server", "-m", (char*) to_string(m).c_str(), nullptr);
    }
	// initialize overhead (including the control channel)
	FIFORequestChannel* chan = new FIFORequestChannel("control", FIFORequestChannel::CLIENT_SIDE);
    BoundedBuffer request_buffer(b);
    BoundedBuffer response_buffer(b);
	HistogramCollection hc;
    //array of producer threads (if data, p elements; if file, 1 element)
    vector<thread> patient_threads;

    //array of FIFOs (w elements)
    vector<FIFORequestChannel*> fifos;
    // array of worker threads (w elements)
    vector<thread> worker_threads;
    //array of histogram threads (if data, h elements; if file, zero elements)
    vector<thread> hist_threads;
  
    // making histograms and adding to collection
    for (int i = 0; i < p; i++) {
        Histogram* h = new Histogram(10, -2.0, 2.0);
        hc.add(h);
    }
	// record start time
    struct timeval start, end;
    gettimeofday(&start, 0);


    /*---------------->create all threads here */
    /*if data:
        - create p patient_threads
    if file: 
        - create 1 file_thread

     create w worker_threads (store in worker)
            -> create w channels (store in fifo array) 

    if data: 
        - create h hist_threads
    */
    //Creating channels prior to any other operation
    for(int i = 0; i < w; i++){
        MESSAGE_TYPE nc = NEWCHANNEL_MSG;
    	chan->cwrite(&nc, sizeof(MESSAGE_TYPE));
		char chan_name[100];
		chan->cread(chan_name, sizeof(chan_name));
		FIFORequestChannel* fifo =  new FIFORequestChannel(chan_name, FIFORequestChannel::CLIENT_SIDE);
        fifos.push_back(fifo);
    }

    if (f == "") {
        //data
        for (int i = 1; i <= p; i++) {
           patient_threads.push_back(thread(patient_thread_function, i, n, &request_buffer));
        }
    } else {
        //file
        patient_threads.push_back(thread(file_thread_function, m, chan, f, &request_buffer));
    }

    for(int i = 0; i < w; i++){
        cout<<"Making New Worker Thread"<<endl;
        worker_threads.push_back(thread(worker_thread_function, m, &request_buffer, &response_buffer, fifos[i]));
    }

    if(f == ""){
        for(int i = 0; i < h; i++){
           hist_threads.push_back(thread(histogram_thread_function, &response_buffer, &hc));
        }
    }




   
	/*----------------> join all threads here */
    // iterate over all thread arrays, callinig join()
    //      - order is important; produers before consumers
    // add quit messages to request buffer
    // join worker threads
    // join histogram threads (if data)

for (long unsigned int i = 0; i < patient_threads.size(); ++i) {
        patient_threads[i].join();
    }
    // quit msg request buffer for his
for (int i = 0; i < w; ++i) { //??? worker size 
        MESSAGE_TYPE q = QUIT_MSG;
        request_buffer.push((char*)&q, sizeof(MESSAGE_TYPE));
    }

for (int i = 0; i < w; ++i) {
        worker_threads[i].join();
    }

    Response* quit_resp = new Response(-1, -1);
    // quit msg repsonse buffer for his
    for (int i = 0; i < h; ++i) { //??? hist size 
       response_buffer.push((char*)quit_resp, sizeof(Response));
    }
    delete quit_resp;

    if(f == ""){
        for (int i = 0; i < h; ++i) {
            hist_threads[i].join();
        }
    }

	// record end time
    gettimeofday(&end, 0);

    // print the results
	if (f == "") {
		hc.print();
	}
    int secs = ((1e6*end.tv_sec - 1e6*start.tv_sec) + (end.tv_usec - start.tv_usec)) / ((int) 1e6);
    int usecs = (int) ((1e6*end.tv_sec - 1e6*start.tv_sec) + (end.tv_usec - start.tv_usec)) % ((int) 1e6);
    cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

    // quit and close all channels in fifo array
    // add in quit message cond in worker, no loop
    for (long unsigned int i = 0; i < fifos.size(); i++) {
        MESSAGE_TYPE q = QUIT_MSG;
        fifos[i]->cwrite ((char *) &q, sizeof (MESSAGE_TYPE));
    }
    for (auto fifo : fifos) {
        delete fifo;
    }
    
    // quit and close control channel
    MESSAGE_TYPE q = QUIT_MSG;
    chan->cwrite ((char *) &q, sizeof (MESSAGE_TYPE));
    delete chan;
	// wait for server to exit
	wait(nullptr);
    
}
