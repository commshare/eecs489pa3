/*
 * Copyright (c) 2014, 2015 University of Michigan, Ann Arbor.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of Michigan, Ann Arbor. The name of the University 
 * may not be used to endorse or promote products derived from this 
 * software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Author: Sugih Jamin (jamin@eecs.umich.edu)
 *
*/
#include <stdio.h>         // fprintf(), perror(), fflush()
#include <stdlib.h>        // atoi(), random()
#include <assert.h>        // assert()
#include <limits.h>        // LONG_MAX, INT_MAX
#include <iostream>
using namespace std;
#include <string.h>        // memset(), memcmp(), strlen(), strcpy(), memcpy()
#include <unistd.h>        // getopt(), STDIN_FILENO, gethostname()
#include <signal.h>        // signal()
#include <netdb.h>         // gethostbyname(), gethostbyaddr()
#include <netinet/in.h>    // struct in_addr
#include <arpa/inet.h>     // htons(), inet_ntoa()
#include <sys/types.h>     // u_short
#include <sys/socket.h>    // socket API, setsockopt(), getsockname()
#include <sys/ioctl.h>     // ioctl(), FIONBIO
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <errno.h>

#include "ltga.h"
#include "socks.h"
#include "netimg.h"
#include "imgdb.h"
#include "fec.h"

#define MINPROB 0.011
#define MAXPROB 0.11

int dummycounter = 0;

/*
 * imgdb_args: parses command line args.
 *
 * Returns 0 on success or 1 on failure.  On successful return,
 * the provided drop probability is copied to memory pointed to by
 * "pdrop", which must be allocated by caller.  
 *
 * Nothing else is modified.
 */
int imgdb::
args(int argc, char *argv[])
{
  char c;
  extern char *optarg;

  if (argc < 1) {
    return (1);
  }
  
  while ((c = getopt(argc, argv, "d:")) != EOF) {
    switch (c) {
    case 'd':
      pdrop = atof(optarg);
      if (pdrop > 0.0 && (pdrop > MAXPROB || pdrop < MINPROB)) {
        fprintf(stderr, "%s: recommended drop probability between %f and %f.\n", argv[0], MINPROB, MAXPROB);
      }
      break;
    default:
      return(1);
      break;
    }
  }

  srandom(NETIMG_SEED+(int)(pdrop*1000));

  return (0);
}

/*
 * readimg: load TGA image from file "imgname" to curimg.
 * "imgname" must point to valid memory allocated by caller.
 * Terminate process on encountering any error.
 * Returns NETIMG_FOUND if "imgname" found, else returns NETIMG_NFOUND.
 */
char imgdb::
readimg(char *imgname, int verbose)
{
  string pathname=IMGDB_FOLDER;

  if (!imgname || !imgname[0]) {
    return(NETIMG_ENAME);
  }
  
  curimg.LoadFromFile(pathname+IMGDB_DIRSEP+imgname);

  if (!curimg.IsLoaded()) {
    return(NETIMG_NFOUND);
  }

  if (verbose) {
    cerr << "Image: " << endl;
    cerr << "       Type = " << LImageTypeString[curimg.GetImageType()] 
         << " (" << curimg.GetImageType() << ")" << endl;
    cerr << "      Width = " << curimg.GetImageWidth() << endl;
    cerr << "     Height = " << curimg.GetImageHeight() << endl;
    cerr << "Pixel depth = " << curimg.GetPixelDepth() << endl;
    cerr << "Alpha depth = " << curimg.GetAlphaDepth() << endl;
    cerr << "RL encoding = " << (((int) curimg.GetImageType()) > 8) << endl;
    /* use curimg.GetPixels()  to obtain the pixel array */
  }
  
  return(NETIMG_FOUND);
}

/*
 * marshall_imsg: Initialize *imsg with image's specifics.
 * Upon return, the *imsg fields are in host-byte order.
 * Return value is the size of the image in bytes.
 *
 * Terminate process on encountering any error.
 */
double imgdb::
marshall_imsg(imsg_t *imsg)
{
  int alpha, greyscale;

  imsg->im_depth = (unsigned char)(curimg.GetPixelDepth()/8);
  imsg->im_width = curimg.GetImageWidth();
  imsg->im_height = curimg.GetImageHeight();
  alpha = curimg.GetAlphaDepth();
  greyscale = curimg.GetImageType();
  greyscale = (greyscale == 3 || greyscale == 11);
  if (greyscale) {
    imsg->im_format = alpha ? GL_LUMINANCE_ALPHA : GL_LUMINANCE;
  } else {
    imsg->im_format = alpha ? GL_RGBA : GL_RGB;
  }

  return((double) (imsg->im_width*imsg->im_height*imsg->im_depth));
}

/* 
 * recvqry: receives an iqry_t packet and stores the client's address
 * and port number in the imgdb::client member variable.  Checks that
 * the incoming iqry_t packet is of version NETIMG_VERS and of type
 * NETIMG_SYNQRY.
 *
 * If error encountered when receiving packet or if packet is of the
 * wrong version or type returns appropriate NETIMG error code.
 * Otherwise returns 0.
 *
 * Nothing else is modified.
*/
char imgdb::
recvqry(int sd, iqry_t *iqry)
{
  int bytes;  // stores the return value of recvfrom()

  /*
   * Lab5 Task 1: Call recvfrom() to receive the iqry_t packet from
   * client.  Store the client's address and port number in the
   * imgdb::client member variable and store the return value of
   * recvfrom() in local variable "bytes".
  */
  /* Lab5: YOUR CODE HERE */
  /* DONE */ 
  socklen_t addr_len = sizeof(client);
  bytes = recvfrom(
      sd,
      (void *) iqry,
      sizeof(*iqry),
      0,
      (struct sockaddr *) &client,
      &addr_len 
  );

  ihdr_t* temp_hdr = (ihdr_t *) iqry;
  fprintf(stderr, "imgdb::recvqry: packet: vers: 0x%x, type: 0x%x, seqn: 0x%x\n",
      temp_hdr->ih_vers, temp_hdr->ih_type, ntohl(temp_hdr->ih_seqn));

  if (bytes != sizeof(iqry_t)) {
    return (NETIMG_ESIZE);
  }
  if (iqry->iq_vers != NETIMG_VERS) {
    return(NETIMG_EVERS);
  }
  if (iqry->iq_type != NETIMG_SYNQRY) {
    return(NETIMG_ETYPE);
  }
  if (strlen((char *) iqry->iq_name) >= NETIMG_MAXFNAME) {
    return(NETIMG_ENAME);
  }

  return(0);
}
  
/* 
 * sendpkt: sends the provided "pkt" of size "size"
 * to imgdb::client using sendto() and wait for an ACK packet.
 * If ACK doesn't return before retransmission timeout,
 * re-send the packet.  Keep on trying for NETIMG_MAXTRIES times.
 *
 * Upon success, i.e., pkt sent without error and ACK returned,
 * the ACK pkt is stored in the provided "ack" variable, which
 * memory must have been allocated by caller and return the
 * return value of sendto(). Otherwise, return 0 if ACK not
 * received or if the received ACK packet is malformed.
 *
 * Nothing else is modified.
*/
int imgdb::
sendpkt(int sd, char *pkt, int size, ihdr_t *ack)
{
  /* PA3 Task 2.1: sends the provided pkt to client as you did in
   * Lab5.  In addition, initialize a struct timeval timeout variable
   * to NETIMG_SLEEP sec and NETIMG_USLEEP usec and wait for read
   * event on socket sd up to the timeout value.  If no read event
   * occurs before the timeout, try sending the packet to client
   * again.  Repeat NETIMG_MAXTRIES times.  If read event occurs
   * before timeout, receive the incoming packet and make sure that it
   * is an ACK pkt as expected.
   */
  /* PA3: YOUR CODE HERE */

  unsigned int i = 0;

  do {
    // Send 'pkt' to client
    int client_size = sizeof(client);

    sendto(
        sd,
        (void *) pkt,
        size,
        0,
        (const struct sockaddr *) &client,
        client_size 
    );
   
    // Wait for acknowledgement
    int max_fd = sd;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sd, &fds);

    struct timeval tv = {NETIMG_SLEEP, NETIMG_USLEEP};

    int select_result = select(
        max_fd + 1,
        &fds,
        NULL,
        NULL,
        &tv
    ); 

    net_assert(select_result == -1, "imgdb::sendpkt() network error while waiting for ACK\n");

    // Check for valid traffic
    if (select_result) {
      // Attempt to read ihdr_t 'ack'
      int recvfrom_result = recvfrom(
          sd,
          (void *) ack,
          sizeof(ihdr_t),
          0,
          (struct sockaddr *) &client,
          (socklen_t *) &client_size 
      );

      net_assert(recvfrom_result == -1, "imgdb::sendpkt() network error while reading NETIMG_ACK packet\n");
      net_assert(ack->ih_vers != NETIMG_VERS, "imgdb::sendpkt() received packet with unknown header while waiting for NETIMG_ACK packet\n");
      net_assert(ack->ih_type != NETIMG_ACK, "imgdb::sendpkt() received packet with unknown type when waiting for NETIMG_ACK packet\n");

      // Convert packet fields to host-byte-order
      ack->ih_size = ntohs(ack->ih_size);
      ack->ih_seqn = ntohl(ack->ih_seqn);

      return 0;
    } 
    
    // else, timeout (for which we either try again or stop if 
    // we've exhausted our attempts)
    
  } while (++i < NETIMG_MAXTRIES);

  return -1;
}

/*
 * sendimg:
 * Send the image contained in *image to the client
 * pointed to by *client. Send the image in
 * chunks of segsize, not to exceed mss, instead of
 * as one single image. With probability pdrop, drop
 * a segment instead of sending it.
 * Lab6: compute and send an accompanying FEC packet
 * for every "fwnd"-full of data.
 *
 * Terminate process upon encountering any error.
 * Doesn't otherwise modify anything.
*/
void imgdb::
sendimg(int sd, imsg_t *imsg, char *image, long imgsize, int numseg)
{
  /// Send header for image packet ////
  /* Prepare imsg for transmission: fill in im_vers and convert
   * integers to network byte order before transmission.  Note that
   * im_type is set by the caller and should not be modified.  Send
   * the imsg packet by calling imgdb::sendpkt().
   */
  imsg->im_vers = NETIMG_VERS;
  imsg->im_width = htons(imsg->im_width);
  imsg->im_height = htons(imsg->im_height);
  imsg->im_format = htons(imsg->im_format);

  // Send the imsg packet to client by calling sendpkt().

  ihdr_t ack;
  int imsg_result = this->sendpkt(sd, (char *) imsg, sizeof(imsg_t), &ack);
  if ((imsg_result == -1) || (ack.ih_seqn != (unsigned int) NETIMG_SYNSEQ)) {
    return;
  }
  
  //// Send image data ////
  if (image) {
    char *ip = image; /* ip points to the start of image byte buffer */
    unsigned int datasize = mss - sizeof(ihdr_t) - NETIMG_UDPIP;
    
    /* Lab5 Task 1:
     * make sure that the send buffer is of size at least mss.
     */
    /* Lab5: YOUR CODE HERE */
    /* DONE */
    int sendbuff = mss;
    socklen_t optlen = sizeof(sendbuff);
    int result = setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &sendbuff, optlen);
    net_assert(result == -1, "imgdb::sendimg: failed to resize send buffer");

    /* Lab5 Task 1:
     *
     * Populate a struct msghdr with information of the destination
     * client, a pointer to a struct iovec array.  The iovec array
     * should be of size NETIMG_NUMIOV.  The first entry of the iovec
     * should be initialized to point to an ihdr_t, which should be
     * re-used for each chunk of data to be sent.
     */
    /* Lab5: YOUR CODE HERE */
    /* DONE */
    ihdr_t ihdr;
    ihdr.ih_vers = NETIMG_VERS;
    ihdr.ih_type = NETIMG_DATA;

    struct iovec iovec_arr[NETIMG_NUMIOV];
    iovec_arr[0].iov_base = (void *) &ihdr;
    iovec_arr[0].iov_len = sizeof(ihdr);
    
    struct msghdr msg;
    msg.msg_name = &client;
    msg.msg_namelen = sizeof(client);
    msg.msg_iov = iovec_arr;
    msg.msg_iovlen = NETIMG_NUMIOV;
    msg.msg_control = 0;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    /* PA3 Task 2.2 and Task 4.1: initialize any necessary variables
     * for your sender side sliding window and FEC window.
     */
    /* PA3: YOUR CODE HERE */
    unsigned int snd_una = 0;
    unsigned int snd_next = 0;
    unsigned int window = this->rwnd;
    unsigned int max_segment = imgsize / datasize;

    unsigned char * fec_buff = new unsigned char[datasize];
    unsigned int fec_window_start = 0;
    unsigned int num_fec_sent = 0;
    unsigned int num_fec_acked = 0;

    do {
      /* PA3 Task 2.2: estimate the receiver's receive buffer based on packets
       * that have been sent and ACKed, including outstanding FEC packet(s).
       * We can only send as much as the receiver can buffer.
       * It's an estimate, so it doesn't have to be exact, being off by
       * one or two packets is fine.
       */
      /* PA3: YOUR CODE HERE */
      unsigned int window_limit = snd_una + window - (num_fec_sent - num_fec_acked); /* usable window */
      unsigned int snd_limit = (window_limit < max_segment + 1)
          ? window_limit
          : max_segment + 1;
      
      // Saturate sending window
      while (snd_next + (num_fec_sent - num_fec_acked) < snd_limit) {
        
        // Compute segment dimensions
        long seqno = snd_next * datasize;
        unsigned int segsize = (imgsize - seqno < datasize)
            ? imgsize - seqno
            : datasize;

        /* Lab6 Task 1:
         *
         * If this is your first segment in an FEC window, use it to
         * initialize your FEC data.  Subsequent segments within the FEC
         * window should be XOR-ed with the content of your FEC data.
         *
         * You MUST use the two helper functions fec.cpp:fec_init()
         * and fec.cpp:fec_accum() to encapsulate your FEC computation
         * for the first and subsequent segments of the FEC window.
         * You are to write these two functions.
         *
         * You need to figure out how to:
         * 1. maintain your FEC window,
         * 2. keep track of your progress over each FEC window, and
         * 3. compute your FEC data across the multiple data segments.
         */
        /* Lab6: YOUR CODE HERE */
        if ((snd_next - fec_window_start) % fwnd) {
          // This is not the first segment in the FEC window, so accumulate 'image' into 'fec_buff'
          fec_accum(fec_buff, (unsigned char *) image + seqno, datasize, segsize);
        } else {
          // Initialize 'fec_buff' b/c this is the first segment we're sending
          fec_init(fec_buff, (unsigned char *) image + seqno, datasize, segsize); 
        }

        // Probabilistically drop segment
        if (((float) random())/INT_MAX < pdrop) { /* drop segment */
          // Report dropped segment
          fprintf(stderr, "imgdb::sendimg: DROPPED DATA packet with offset 0x%lx, %d bytes\n",
                  seqno, segsize);

        } else { /* send segment */
          /* Lab5 Task 1: 
           * Send one segment of data of size segsize at each iteration.
           * Point the second entry of the iovec to the correct offset
           * from the start of the image.  Update the sequence number
           * and size fields of the ihdr_t header to reflect the byte
           * offset and size of the current chunk of data.  Send
           * the segment off by calling sendmsg().
           */
          /* Lab5: YOUR CODE HERE */
          /* DONE */

          ihdr.ih_size = htons(segsize);
          ihdr.ih_seqn = htonl((unsigned int)seqno);
          iovec_arr[1].iov_base = (void *) (ip + seqno);
          iovec_arr[1].iov_len = segsize;

          // Fail due to bad segment send
          net_assert(sendmsg(sd, &msg, 0) == -1, "imgdb::sendimg() Failed to send message");
         
          // Report segment send
          fprintf(stderr, "imgdb::sendimg: sent DATA packet 0x%lx, %d bytes\n",
                  seqno, segsize);
        }

        seqno = ++snd_next * datasize;
        if (seqno > imgsize) {
          seqno = imgsize;
        }
      
        /* Lab6 Task 1
         *
         * If one fwnd-full of fec has been accumulated or last chunk
         *   of data has just been sent, send fec
         *
         * Point the second entry of the iovec to your FEC data.
         * The sequence number of the FEC packet MUST be the sequence
         * number of the first image data byte beyond the FEC window.
         * The size of an FEC packet MUST be "datasize".
         * Don't forget to use network byte order.
         * Send the FEC packet off by calling sendmsg().
         *
         * After you've sent off your FEC packet, you may want to
         * reset your FEC-window related variables to prepare for the
         * processing of the next window.  If you re-use the same header
         * for sending image data and FEC data, make sure you reset the
         * ih_type field of the header also.
         */
        /* Lab6: YOUR CODE HERE */
        if ((snd_next - fec_window_start) % fwnd == 0 || seqno == imgsize) {
          // Check if we should drops the packet
          if (((float) random())/INT_MAX < pdrop) {
            fprintf(stderr, "imgdb::sendimg: DROPPED FEC packet 0x%lx, 0x%x bytes\n",
                    seqno, segsize);
          } else {
            // Prepare for FEC send
            ihdr.ih_type = NETIMG_FEC | NETIMG_DATA;
            ihdr.ih_size = htons( (unsigned short) datasize);
            ihdr.ih_seqn = htonl( (unsigned int) seqno);

            iovec_arr[1].iov_base = (void *) fec_buff;
            iovec_arr[1].iov_len = datasize;
            
            fprintf(stderr, "imgdb::sendimg: sent FEC packet 0x%lx, %d bytes\n",
                    seqno, datasize);

            int fec_send_result = sendmsg(sd, &msg, 0);
            net_assert(fec_send_result == -1, "Failed to send FEC to client");

            // Prepare for DATA send
            ihdr.ih_type = NETIMG_DATA;
          }

          // Adjust "usable window" for FEC packet
          assert(num_fec_sent >= num_fec_acked);
          ++num_fec_sent;

          // Reposition FEC window
          fec_window_start = snd_next;
        }
      }

      // Await ACKS from receiver w/timeout
      int max_fd = sd;
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(sd, &fds);

      struct timeval tv = {NETIMG_SLEEP, NETIMG_USLEEP};

      int select_result = select(
          max_fd + 1,
          &fds,
          NULL,
          NULL,
          &tv
      );

      // Fail due to network error while waiting for ACKs
      net_assert(
          select_result == -1,
          "imgdb::sendimg() network error while waiting for ACK\n"
      );

      // Process ACK traffic
      if (select_result) { 
        // We've received traffic! =>  Optimistically receive all ACKs and update window
        bool more_acks = true;
        
        do {
          // Read more ACKs in non-blocking fashion
          socklen_t addr_len = sizeof(client);
          ihdr_t ack; 
          int ack_result = recvfrom(
              sd,
              (void *) &ack,
              sizeof(ihdr_t),
              MSG_DONTWAIT,
              (struct sockaddr *) &client,
              &addr_len
          );

          if (ack_result == -1) {
            // Fail due to network error when reading ACKs opportunistically
            net_assert(
                errno != EAGAIN && errno != EWOULDBLOCK,
                "imgdb::sendimg() encountered error while opportunistically reading ACKs\n"
            );  

            // We've read all of the ACKs
            more_acks = false;

          } else { /* We've received an ACK, process it */
            // Fail due to invalid vers 
            net_assert(
                ack.ih_vers != NETIMG_VERS,
                "imgdb::sendimg() encountered packet with invalid vers while waiting for ACK\n"
            );

            // Fail due to invalid type
            net_assert(
                ack.ih_type != NETIMG_ACK,
                "imgdb::sendimg() encountered packet with invalid type while waiting for ACK\n"
            );

            // Advance sliding window, if ACK acknowledges new segments
            unsigned int ack_seg = (ntohl(ack.ih_seqn) + datasize - 1) / datasize;

            // Check if client has ACKed new segments
            if (ack_seg > snd_una) {
              snd_una = ack_seg; 
            }

            // Detect ACKed FEC packets
            num_fec_acked = ack_seg / this->fwnd;

            fprintf(stderr, "imgdb::sendimg: received ack: seqn: 0x%x\n", ntohl(ack.ih_seqn));
          }
        } while (more_acks);

      } else { /* We haven't received any traffic => RTO! */
        // Report GBN RTO!
        fprintf(stderr, "imgdb::sendimg: RTO! Retransmitting...\n");

        // Retransmit an entire window-full of segments starting at 'snd_una'
        snd_next = snd_una; 
        fec_window_start = snd_una;
        num_fec_sent = num_fec_acked;
      }


    } while (snd_una <= max_segment); // iterate until we've acknowledged all pkts

    fprintf(stderr, "imgdb::sendimg: Sent FIN packet (0x%lx)\n", NETIMG_FINSEQ);
    
    // Send NETIMG_FIN packet
    ihdr_t fin = {
        NETIMG_VERS,
        NETIMG_FIN,
        0,
        htonl(NETIMG_FINSEQ)
    };

    ihdr_t ack;
    this->sendpkt(sd, (char *) &fin, sizeof(ihdr_t), &ack);

    delete[] fec_buff;
    fec_buff = nullptr;
  }
}

/*
 * handleqry: accept connection, then receive a query packet, search
 * for the queried image, and reply to client.
 */
void imgdb::
handleqry()
{
  iqry_t iqry;
  imsg_t imsg;
  double imgdsize;

  imsg.im_type = recvqry(sd, &iqry);
  if (imsg.im_type) {
    sendimg(sd, &imsg, NULL, 0, 0);
  } else {
    
    imsg.im_type = readimg(iqry.iq_name, 1);
    
    if (imsg.im_type == NETIMG_FOUND) {

      mss = (unsigned short) ntohs(iqry.iq_mss);
      // Lab6:
      rwnd = iqry.iq_rwnd;
      fwnd = iqry.iq_fwnd;

      imgdsize = marshall_imsg(&imsg);
      net_assert((imgdsize > (double) LONG_MAX),
                 "imgdb: image too big");
      sendimg(sd, &imsg, (char *) curimg.GetPixels(),
              (long)imgdsize, 0);
    } else {
      sendimg(sd, &imsg, NULL, 0, 0);
    }
  }

  return;
}

int
main(int argc, char *argv[])
{ 
  socks_init();

  imgdb imgdb;
  
  // parse args, see the comments for imgdb::args()
  if (imgdb.args(argc, argv)) {
    fprintf(stderr, "Usage: %s [ -d <drop probability> ]\n",
            argv[0]); 
    exit(1);
  }

  while (1) {
    imgdb.handleqry();
  }
    
#ifdef _WIN32
  WSACleanup();
#endif // _WIN32
  
  exit(0);
}
