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
#include <stdlib.h>        // atoi()
#include <assert.h>        // assert()
#include <limits.h>        // LONG_MAX
#include <math.h>          // ceil()
#include <errno.h>
#include <string.h>        // memset(), memcmp(), strlen(), strcpy(), memcpy()
#include <unistd.h>        // getopt(), STDIN_FILENO, gethostname()
#include <signal.h>        // signal()
#include <netdb.h>         // gethostbyname()
#include <netinet/in.h>    // struct in_addr
#include <arpa/inet.h>     // htons()
#include <sys/types.h>     // u_short
#include <sys/socket.h>    // socket API
#include <sys/ioctl.h>     // ioctl(), FIONBIO
#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include "netimg.h"
#include "socks.h"
#include "fec.h"          // Lab6

#include <unistd.h>
#include <fcntl.h>

int sd;                   /* socket descriptor */
imsg_t imsg;
long img_size;
unsigned char *image;
unsigned short mss;       // receiver's maximum segment size, in bytes
unsigned char rwnd;       // receiver's window, in packets, of size <= mss
unsigned char fwnd;       // Lab6: receiver's FEC window < rwnd, in packets
float pdrop;

unsigned int nextSeqNo_ = 0;
unsigned int firstMissingSeqNo_;
unsigned int numFecSegs_ = 0;
unsigned int currFecSeqNo_ = 0;
unsigned int bytesReceived_ = 0;

unsigned int numMissingSegs_ = 0;

bool inGbnMode_ = false;

/*
 * netimg_args: parses command line args.
 *
 * Returns 0 on success or 1 on failure.  On successful return,
 * "*sname" points to the server's name, and "port" points to the port
 * to connect at server, in network byte order.  Both "*sname", and
 * "port" must be allocated by caller.  The variable "*imgname" points
 * to the name of the image to search for.  The global variables mss,
 * rwnd, and pdrop are initialized.
 *
 * Nothing else is modified.
 */
int
netimg_args(int argc, char *argv[], char **sname, u_short *port, char **imgname)
{
  char c, *p;
  extern char *optarg;
  int arg;

  if (argc < 5) {
    return (1);
  }
  
  pdrop = NETIMG_PDROP;
  rwnd = NETIMG_RCVWIN;
  mss = NETIMG_MSS;

  while ((c = getopt(argc, argv, "s:q:w:m:d:")) != EOF) {
    switch (c) {
    case 's':
      for (p = optarg+strlen(optarg)-1;  // point to last character of
                                         // addr:port arg
           p != optarg && *p != NETIMG_PORTSEP;
                                         // search for ':' separating
                                         // addr from port
           p--);
      net_assert((p == optarg), "netimg_args: server address malformed");
      *p++ = '\0';
      *port = htons((u_short) atoi(p)); // always stored in network byte order

      net_assert((p-optarg > NETIMG_MAXFNAME),
                 "netimg_args: server's name too long");
      *sname = optarg;
      break;
    case 'q':
      net_assert((strlen(optarg) >= NETIMG_MAXFNAME),
                 "netimg_args: image name too long");
      *imgname = optarg;
      break;
    case 'w':
      arg = atoi(optarg);
      if (arg < NETIMG_MINWIN || arg > NETIMG_MAXWIN) {
        return(1);
      }
      rwnd = (unsigned char) arg; 
      break;
    case 'm':
      arg = atoi(optarg);
      if (arg < NETIMG_MINSS || arg > NETIMG_MSS) {
        return(1);
      }
      mss = (unsigned short) arg;
      break;
    case 'd':
      pdrop = atof(optarg);  // global
      if (pdrop > 0.0 && (pdrop > NETIMG_MAXPROB || pdrop < NETIMG_MINPROB)) {
        fprintf(stderr, "%s: recommended drop probability between %f and %f.\n",
                argv[0], NETIMG_MINPROB, NETIMG_MAXPROB);
      }
      break;
    default:
      return(1);
      break;
    }
  }

  return (0);
}


/*
 * netimg_sendqry: send a query for provided imgname to
 * connected server.  Query is of type iqry_t, defined in netimg.h.
 * The query packet must be of version NETIMG_VERS and of type
 * NETIMG_SYNQRY both also defined in netimg.h. In addition to the
 * filename of the image the client is searching for, the query
 * message also carries the receiver's window size (rwnd), maximum
 * segment size (mss), and FEC window size (used in Lab6).
 * All three are global variables.
 *
 * On send error, return 0, else return 1
 */
int
netimg_sendqry(char *imgname)
{

  fprintf(stderr, " - Sending query\n");
  int bytes;
  iqry_t iqry;

  iqry.iq_vers = NETIMG_VERS;
  iqry.iq_type = NETIMG_SYNQRY;
  iqry.iq_mss = htons(mss);      // global
  iqry.iq_rwnd = rwnd;           // global
  iqry.iq_fwnd = fwnd = NETIMG_FECWIN >= rwnd ? rwnd-1 : NETIMG_FECWIN;  // Lab6
  strcpy(iqry.iq_name, imgname); 
  bytes = send(sd, (char *) &iqry, sizeof(iqry_t), 0);
  if (bytes != sizeof(iqry_t)) {
    return(0);
  }

  return(1);
}
  
/*
 * netimg_recvimsg: receive an imsg_t packet from server and store it
 * in the global variable imsg.  The type imsg_t is defined in
 * netimg.h. Return NETIMG_EVERS if packet is of the wrong version.
 * Return NETIMG_ESIZE if packet received is of the wrong size.
 * Otherwise return the content of the im_type field of the received
 * packet. Upon return, all the integer fields of imsg MUST be in HOST
 * BYTE ORDER. If msg_type is NETIMG_FOUND, compute the size of the
 * incoming image and store the size in the global variable
 * "img_size".
 */
char
netimg_recvimsg()
{
  int bytes;
  double imgdsize;

  /* receive imsg packet and check its version and type */
  bytes = recv(sd, (char *) &imsg, sizeof(imsg_t), 0); // imsg global
  if (bytes != sizeof(imsg_t)) {
    return(NETIMG_ESIZE);
  }
  if (imsg.im_vers != NETIMG_VERS) {
    return(NETIMG_EVERS);
  }

  if (imsg.im_type == NETIMG_FOUND) {
    imsg.im_height = ntohs(imsg.im_height);
    imsg.im_width = ntohs(imsg.im_width);
    imsg.im_format = ntohs(imsg.im_format);

    imgdsize = (double) (imsg.im_height*imsg.im_width*(u_short)imsg.im_depth);
    net_assert((imgdsize > (double) LONG_MAX), 
               "netimg_recvimsg: image too big");
    img_size = (long) imgdsize;                 // global

    /* PA3 Task 2.1:
     *
     * Send back an ACK with ih_type = NETIMG_ACK and ih_seqn =
     * NETIMG_SYNSEQ.  Initialize any variable necessary to keep track
     * of ACKs.
     *
     * TODO: initialize variables to track ACKs
     */
    /* PA3: YOUR CODE HERE */
    ihdr_t ih;
    ih.ih_vers = NETIMG_VERS;
    ih.ih_type = NETIMG_ACK;
    ih.ih_seqn = htonl(NETIMG_SYNSEQ);

    int ack_result = send(sd, (void *) &ih, sizeof(ihdr_t), 0);
    net_assert(ack_result == -1, "netimg_recvimsg() network error while sending NETIMG_ACK back to server\n");
  }

  return((char) imsg.im_type);
}

/* Callback function for GLUT.
 *
 * netimg_recvimg: called by GLUT when idle On each call, receive a
 * chunk of the image from the network and store it in global variable
 * "image" at offset from the start of the buffer as specified in the
 * header of the packet.
 *
 * Terminate process on receive error.
 */
void
netimg_recvimg(void)
{

  ihdr_t hdr;  // memory to hold packet header
   
  /* 
   * Lab5 Task 2:
   * 
   * The image data packet from the server consists of an ihdr_t
   * header followed by a chunk of data.  We want to put the data
   * directly into the buffer pointed to by the global variable
   * "image" without any additional copying. To determine the correct
   * offset from the start of the buffer to put the data into, we
   * first need to retrieve the sequence number stored in the packet
   * header.  Since we're dealing with UDP packet, however, we can't
   * simply read the header off the network, leaving the rest of the
   * packet to be retrieved by subsequent calls to recv(). Instead, we
   * call recv() with flags == MSG_PEEK.  This allows us to retrieve a
   * copy of the header without removing the packet from the receive
   * buffer.
   *
   * Since our socket has been set to non-blocking mode, if there's no
   * packet ready to be retrieved from the socket, the call to recv()
   * will return immediately with return value -1 and the system
   * global variable "errno" set to EAGAIN or EWOULDBLOCK (defined in
   * errno.h).  In which case, this function should simply return to
   * caller.
   * 
   * Once a copy of the header is made to the local variable "hdr",
   * check that it has the correct version number and that it is of
   * type NETIMG_DATA (use bitwise '&' as NETIMG_FEC is also of type
   * NETIMG_DATA).  Terminate process if any error is encountered.
   * Otherwise, convert the size and sequence number in the header
   * to host byte order.
   */
  /* Lab5: YOUR CODE HERE */
  /* DONE */
  int result = recv(sd, (void *) &hdr, sizeof(hdr), MSG_PEEK);
  if (result == -1) {
    net_assert(errno != EAGAIN && errno != EWOULDBLOCK, "Unknown error during netimg recv");
    return;
  }

  net_assert(hdr.ih_vers != NETIMG_VERS, "Invalid ihdr_t version");
  if (!(NETIMG_DATA & hdr.ih_type)) {
    fprintf(stderr, "DEBUG: hdr.ih_type: 0x%x\n", hdr.ih_type);
  }
  net_assert(!(NETIMG_DATA & hdr.ih_type), "Invalid ihdr_t type received!");

  uint16_t size = ntohs(hdr.ih_size);
  uint32_t seqn = ntohl(hdr.ih_seqn);

  /* Lab5 Task 2
   *
   * Populate a struct msghdr with a pointer to a struct iovec
   * array.  The iovec array should be of size NETIMG_NUMIOV.  The
   * first entry of the iovec should be initialized to point to the
   * header above, which should be re-used for each chunk of data
   * received.
   */
  /* Lab5: YOUR CODE HERE */
  /* DONE */
  struct iovec iovec_arr[NETIMG_NUMIOV];
  iovec_arr[0].iov_base = (void *) &hdr;
  iovec_arr[0].iov_len = sizeof(hdr);


  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iovec_arr;
  msg.msg_iovlen = NETIMG_NUMIOV;

  if (hdr.ih_type == NETIMG_DATA) {
    // Report that we've received a DATA segment   
    fprintf(
        stderr,
        "netimg_recvimg: received DATA pkt 0x%x, %d bytes.\n",
        seqn,
        size
    );

    /* 
     * Lab5 Task 2
     *
     * Now that we have the offset/seqno information from the packet
     * header, point the second entry of the iovec to the correct
     * offset from the start of the image buffer pointed to by the
     * global variable "image".  Both the offset/seqno and the size of
     * the data to be received into the image buffer are recorded in
     * the packet header retrieved above. Receive the segment "for
     * real" (as opposed to "peeking" as we did above) by calling
     * recvmsg().  We'll be overwriting the information in the "hdr"
     * local variable, so remember to convert the size and sequence
     * number in the header to host byte order again.
     */
    /* Lab5: YOUR CODE HERE */
    /* DONE */
    iovec_arr[1].iov_base = (void *) (image + seqn);
    iovec_arr[1].iov_len = size;

    result = recvmsg(sd, &msg, 0);
    net_assert(result == -1, "Failed to read DATA from wire");

    // Check if we're in GBN mode
    if (inGbnMode_) { 
      if (seqn == firstMissingSeqNo_) { /* we've received the dropped segment, now exit GBN mode*/
        // Report that we're reactivating FEC and repositioning the FEC window
        fprintf(
            stderr,
            "\t- Received expected retransmitted packet! Exiting GBN mode. Repositioning FEC window to start at 0x%x\n",
            firstMissingSeqNo_
        );

        // Exit GBN mode
        inGbnMode_ = false; 

        // Reposition FEC window
        currFecSeqNo_ = firstMissingSeqNo_;
        nextSeqNo_ = firstMissingSeqNo_;
        numFecSegs_ = 0;
        numMissingSegs_ = 0;

      } else { /* still waiting for the dropped segment, ignore this segment */
        // Report that we're ignoring this segment
        fprintf(
            stderr,
            "\t- Ignoring packet b/c we're in GBN mode and waiting for seqn: 0x%x\n",
            nextSeqNo_
        );
      } 
    }

    /* Lab6 Task 2
     *
     * You should handle the case when the FEC data packet itself may be
     * lost, and when multiple packets within an FEC window are lost, and
     * when the first few packets from the subsequent FEC window following a
     * lost FEC data packet are also lost.  Thus in In addition to relying on
     * fwnd and the count of total packets received within an FEC
     * window, you may want to rely on the sequence numbers in arriving
     * packets to determine when you have received an FEC-window full of data
     * bytes.
     *
     * To that end, in addition to keeping track of lost packet offset
     * below, every time a data packet arrives, first check whether
     * you have received an FEC-window full (or more) of data bytes
     * without receiving any FEC packet.  In which case, you need to
     * reposition your FEC window by computing the start of the
     * current FEC window, reset your count of packets received, and
     * determine the next expected packet.
     *
     * Check whether the arriving data packet is the next data packet
     * you're expecting.  If not, you've lost a packet, mark the
     * location of the first lost packet in an FEC window.  If more
     * than one packet is lost, you don't need to mark subsequent
     * losses, just keep a count of the total number of packets received.
     */
    /* Lab6: YOUR CODE HERE */

    // Only process DATA packets if we're in FEC mode
    if (!inGbnMode_) {
      // Check if we've dropped an FEC pkt 
      unsigned int datasize = mss - sizeof(ihdr_t) - NETIMG_UDPIP;
      unsigned int feq_size_bytes = datasize * fwnd; // size of an FEC window in bytes
      unsigned int next_fec_seq_no = feq_size_bytes + currFecSeqNo_;

      if (seqn < nextSeqNo_) { /* ignore packet */
        // Report that we're ignoring this packet
        fprintf(
            stderr,
            "\t- Ignoring packet b/c its seqn is less than the one we're waiting for, 0x%x\n",
            nextSeqNo_
        );

      } else { /* process packet */

        // Check if we missed an FEC packet
        if (nextSeqNo_ == next_fec_seq_no) { /* we've dropped an FEC pkt */
          
          // Determine if we can recover from the dropped FEC packet
          if (numMissingSegs_ == 1) { /* can't recover missing segment => GBN */
            // Report GBN transition
            fprintf(
                stderr,
                "\t- FEC packet seqn: 0x%x was dropped along with DATA segment 0x%x from the same FEC window. Transitioning to GBN...\n",
                nextSeqNo_,
                firstMissingSeqNo_
            );

            // Transition to GBN mode
            inGbnMode_ = true; 
          
          } else { /* we don't need the FEC packet, so proceed */
            // Report that we can ignore this FEC packet
            fprintf(
                stderr,
                "\t- FEC packet 0x%x was dropped, but we already have all of the DATA segments in its FEC window, therefore we can ignore it and advance our FEC window from 0x%x to 0x%x\n",
                nextSeqNo_,
                currFecSeqNo_,
                nextSeqNo_
            );

            // Advance FEC window
            currFecSeqNo_ = nextSeqNo_;
          }
        }

        // At this point, we might have transitioned into GBN or we have more dropped
        // packets to process or we have the expected segment.

        // Ignore if we're in GBN mode
        if (!inGbnMode_) {
         
          // Enumerate remaining dropped packts, if any
          unsigned int next_seqno = nextSeqNo_;

          while (next_seqno < seqn) {
            // Check if this is a dropped FEC packet that we haven't processed yet
            if ((next_seqno - currFecSeqNo_) % fwnd == 0 
                && currFecSeqNo_ != next_seqno) {
              
              // Report that we've dropped this FEC segment
              fprintf(
                  stderr,
                  "\t- Dropped FEC segment 0x%x\n",
                  next_seqno
              );

              ++numMissingSegs_;

              // Must fail if we're missing a FEC here. FEC can't be the first
              // segment that we're missing here b/c we would've handled this case
              // earlier. Can't be greater than 2, b/c that would've triggered
              // GBN already.
              assert(numMissingSegs_ != 2);

              // Report that we're going into GBN mode
              fprintf(
                  stderr,
                  "\t- FEC packet 0x%x was dropped along with a DATA packet from the same FEC window, 0x%x. Entering GBN mode.\n",
                  next_seqno,
                  firstMissingSeqNo_
              );

              // Transition to GBN mode and short circuit
              inGbnMode_ = true;
              break;
            }

            // Report that we've dropped a DATA segment
            fprintf(
                stderr,
                "\t- Dropped DATA segment 0x%x\n",
                next_seqno
            );

            ++numMissingSegs_;

            // Check if this is the first missing segment 
            if (numMissingSegs_ == 1) {
              firstMissingSeqNo_ = next_seqno;
            
            } else if (firstMissingSeqNo_ + datasize == next_seqno) { /* check for consecutive segment drops */
              
              // Report that we've dropped multiple segments and that we're
              // transitioning to GBN
              fprintf(
                  stderr,
                  "\t- Dropped two consecutive segments: first segment 0x%x, second segment 0x%x. Entering GBN mode.\n",
                  firstMissingSeqNo_,
                  next_seqno
              );

              // Transition to GBN mode and short circuit
              inGbnMode_ = true;
              break;
            }
            
            unsigned int dropped_pkt_size = (datasize > img_size - next_seqno)
                ? img_size - next_seqno 
                : datasize;

            next_seqno += dropped_pkt_size;
          }
        }

        // At this point, we might have transitioned into GBN mode or we might be ready to
        // ACK for the proper segment or a missing segment.

        if (!inGbnMode_) {
          // Send ACK to server
          unsigned int ack_seq = 0;
         
          // Send ACK to server
          if (numMissingSegs_) { /* ACK for the missing segment */
            ack_seq = firstMissingSeqNo_;
         
            // Report ACK for missing segment
            fprintf(
                stderr,
                "\t- Sending ACK 0x%x to server for missing segment. Total missing segments: %u.\n",
                ack_seq,
                numMissingSegs_
            );

          } else { /* ACK for the next segment */

//fprintf(stderr, "1DEBUG: next-seqno: 0x%x, img-size: 0x%lx, size: 0x%x\n", nextSeqNo_, img_size, size);

            // Advance next-seq-number to next expected segment
            nextSeqNo_ += (datasize > img_size - nextSeqNo_)
                ? img_size - nextSeqNo_
                : datasize;
//fprintf(stderr, "2DEBUG: next-seqno: 0x%x, img-size: 0x%lx, size: 0x%x\n", nextSeqNo_, img_size, size);
            
            ack_seq = nextSeqNo_; 
          
            // Check if expected segment was a missing segment
            if (numMissingSegs_ && firstMissingSeqNo_ == nextSeqNo_) {

              // Report that we've found the missing segment!
              fprintf(
                  stderr,
                  "\t- We've received the missing segment! Number of missing segments going from %u to %u\n",
                  numMissingSegs_,
                  0
              );

              // Reset num-missing-segs count
              numMissingSegs_ = 0;
            }
            
            // Report ACK for next segment 
            fprintf(
                stderr,
                "\t- Sending ACK 0x%x to server for next segment.\n",
                ack_seq
            );
          }
          
          ihdr_t ack = {
              NETIMG_VERS,
              NETIMG_ACK,
              0,
              htonl(ack_seq)
          };
          
          // Probabilistically send ACK
          if (((float) random())/INT_MAX < pdrop) {
            // Report dropped ACK 
            fprintf(stderr, "\t\t- DROPPED ACK 0x%x\n", ack_seq);

          } else { /* send segment */
            int ack_result = send(sd, (void *) &ack, sizeof(ihdr_t), 0);
            net_assert(ack_result == -1, "netimg_recvimg() failed to send ACK packet to server\n");
          }
        }
      }
    }
    
  } else if (hdr.ih_type == NETIMG_FEC) { // FEC pkt
    /* Lab6 Task 2
     *
     * Re-use the same struct msghdr above to receive an FEC packet.
     * Point the second entry of the iovec to your FEC data buffer and
     * update the size accordingly.  Receive the segment by calling
     * recvmsg().
     *
     * Convert the size and sequence number in the header to host byte
     * order.
     *
     * This is an adaptation of your Lab5 code.
     */
    /* Lab6: YOUR CODE HERE */
    fprintf(stderr, "netimg_recvimg: received FEC offset 0x%x, %d bytes\n",
            seqn, size);
    
    unsigned int datasize = mss - sizeof(ihdr_t) - NETIMG_UDPIP;
    net_assert(datasize != size, "size does not equal datasize");

    // Receive FEC packet into buffer
    unsigned char * fec_buff = new unsigned char[datasize];
    memset(fec_buff, 0, datasize);
    iovec_arr[1].iov_base = (void *) fec_buff;
    iovec_arr[1].iov_len = size;
    int fec_result = recvmsg(sd, &msg, 0);
    net_assert(fec_result == -1, "Failed to read FEC pkt from wire");

    /* Lab6 Task 2
     *
     * Check if you've lost only one packet within the FEC window, if
     * so, reconstruct the lost packet.  Remember that we're using the
     * image data buffer itself as our FEC buffer and that you've
     * noted above the sequence number that marks the start of the
     * current FEC window.  To reconstruct the lost packet, use
     * fec.cpp:fec_accum() to XOR the received FEC data against the
     * image data buffered starting from the start of the current FEC
     * window, one <tt>datasize</tt> at a time, skipping over the lost
     * segment, until you've reached the end of the FEC window.  If
     * fec_accum() has been coded correctly, it should be able to
     * correcly handle the case when the last segment of the
     * FEC-window is smaller than datasize *(but you must still do the
     * detection for short last segment here and provide fec_accum()
     * with the appropriate segsize)*.
     *
     * Once you've reconstructed the lost segment, copy it from the
     * FEC data buffer to correct offset on the image buffer.  You
     * must be careful that if the lost segment is the last segment of
     * the image data, it may be of size smaller than datasize, in
     * which case, you should copy only the correct amount of bytes
     * from the FEC data buffer to the image data buffer.  If no
     * packet was lost in the current FEC window, or if more than one
     * packets were lost, there's nothing further to do with the
     * current FEC window, just move on to the next one.
     *
     * Before you move on to the next FEC window, you may want to
     * reset your FEC-window related variables to prepare for the
     * processing of the next window.
     */
    /* Lab6: YOUR CODE HERE */

    // Check for skipped data segments
    if (!inGbnMode_) {
      
      // Check for dropped pkts
      if (seqn > nextSeqNo_) {
  
        // Enumerate all dropped pkts
        unsigned int next_seqno = nextSeqNo_;

        while (next_seqno < seqn) {
          //fprintf(stderr, "DEBUG: next-seqno: 0x%x, seqn: 0x%x\n", next_seqno, seqn);
          // Determine type of dropped packet
          if ((next_seqno - currFecSeqNo_) % fwnd == 0) { /* dropped packet is FEC */
            // Report dropped FEC pkt
            fprintf(stderr, "\t- Dropped FEC pkt 0x%x\n", next_seqno);

            // Check if we've dropped the FEC packet of a complete DATA window
            if (numMissingSegs_ == 0) { /* Skip dropped FEC pkt, b/c we don't need it*/
              // Report that we're ignoring this dropped FEC packet
              fprintf(
                  stderr,
                  "\t\t- Ignoring dropped FEC pkt b/c we already have all of the DATA packets for that FEC window. Repositioning FEC window to 0x%x\n",
                 next_seqno 
              );

              // Reposition FEC window
              currFecSeqNo_ = next_seqno;

            } else { /* register dropped FEC pkt */
             
              // Check for missing DATA segment
              if (numMissingSegs_) {
                // Report GBN mode transition
                fprintf(
                    stderr,
                    "\t- Entering GBN mode b/c we dropped FEC pkt 0x%x and DATA pkt 0x%x from the same FEC window.\n",
                    next_seqno,
                    firstMissingSeqNo_
                );

                // Enter GBN mode
                inGbnMode_ = true;
                break;
              }
            }
          }
          
          // Report dropped DATA pkt
          fprintf(stderr, "\t- Dropped DATA pkt: offset: 0x%x\n", next_seqno); 
         
          // Track number of missing segements
          ++numMissingSegs_;

          // Register first missing DATA segment
          if (numMissingSegs_ == 1) {
            firstMissingSeqNo_ = next_seqno;
          }

          // Check for consecutive missing segments
          if (numMissingSegs_ == 2 && firstMissingSeqNo_ == next_seqno - datasize) {
            // Report client is IMMEDIATELY entering GBN mode
            fprintf(
                stderr,
                "\t- Encountered consecutive missing DATA segments: first seqn (DATA): 0x%x, second seqn (DATA): 0x%x\n",
                firstMissingSeqNo_,
                next_seqno
            );   
           
            // Enter GBN mode
            inGbnMode_ = true;
            break; /* short circuit for consecutive drops */
          }

          next_seqno += datasize;
        }

        // Put client in GBN mode b/c more than one packet has been lost
        if (!inGbnMode_) {
         
          // Check if we have multiple missing segments within this FEC window
          if (numMissingSegs_ > 1) {
            // Report GBN transition due to multiple missing segs within the same FEC window
            fprintf(
                stderr,
                "\t- Entering GBN mode b/c %u packet(s) have been lost within the same FEC window.\n",
                numMissingSegs_
            );
            
            // Trigger GBN mode
            inGbnMode_ = true;
          } 
        }
      }

      // Advance FEC window (recovered single dropped pkt or received all pkts)
      if (!inGbnMode_) {
        // Check if we can recover a lost packet
        if (numMissingSegs_ == 1) { /* recover lost packet */
          // Report DATA packet recovery
          fprintf(stderr, "\t- All but one DATA packet received! Recovering DATA packet 0x%x\n", firstMissingSeqNo_); 
          
          // Recover missing packet
          unsigned int last_segment_seqno = img_size - (img_size % datasize);
          
          for (unsigned int i = currFecSeqNo_; i < seqn; i += datasize) {
            // Don't XOR the missing segment
            if (i != firstMissingSeqNo_) {
              // Determine proper segsize (might be last segment...)
              unsigned int segsize = (i == last_segment_seqno)
                  ? img_size % datasize
                  : datasize;

              // DEBUG
              fprintf(
                  stderr,
                  "\t- XOR details: seqn: 0x%x, datasize: %u, segsize: %u\n",
                  i,
                  datasize,
                  segsize
              );
              
              // Perform simple XOR magic on the image DATA segments and the FEC data
              // in order to recover the missing segment
              fec_accum(fec_buff, image + i, datasize, segsize);
            }
          }

          // Copy the reconstructed pkt to its proper place in the image buffer
          unsigned int missing_segment_size = (firstMissingSeqNo_ == last_segment_seqno)
              ? img_size - last_segment_seqno
              : datasize;
          
          memcpy(image + firstMissingSeqNo_, fec_buff, missing_segment_size);

          --numMissingSegs_;

          //fprintf(stderr, "DEBUG: last-segment-seqno: 0x%x, next-seqno: 0x%x, img-size: 0x%lx, datasize: 0x%x\n", last_segment_seqno, nextSeqNo_, img_size, datasize);

          // Advance the next expected sequence number
          nextSeqNo_ += (last_segment_seqno == nextSeqNo_)
              ? img_size - nextSeqNo_
              : datasize; 

          // Send back ACK for FEC packet 
          ihdr_t ack = {
              NETIMG_VERS,
              NETIMG_ACK,
              0,
              htonl(nextSeqNo_)
          };
          
          // Probabilistically send FEC ACK
          if (((float) random())/INT_MAX < pdrop) { /* drop ack */
            // Report dropped ACK 
            fprintf(stderr, "imgdb_sendimg: DROPPED ACK for FEC packet seqn: 0x%x\n", nextSeqNo_);

          } else { /* send ack */
            int ack_result = send(sd, (void *) &ack, sizeof(ihdr_t), 0);
            net_assert(ack_result == -1, "netimg_recvimg() failed to send FEC ACK packet to server\n");
          }

          // Unregister missing segment that we just recovered
          numMissingSegs_ = 0;

        } else { /* all packet received, nothing to be done */
          assert(numMissingSegs_ == 0); /* should be in gbn mode, otherwise */

          // Report that all data pkts have been received
          fprintf(stderr, "\t- All DATA packets received! No recovery necessary.\n");
        }

        // Advance FEC window
        numFecSegs_ = 0;
        currFecSeqNo_ = seqn;
    
        //fprintf(stderr, "DEBUG: next-seq-no: 0x%x, seqn: 0x%x\n", nextSeqNo_, seqn);
        assert(nextSeqNo_ == seqn);
        assert(numMissingSegs_ == 0);
      }

      // else, we're in GBN mode and should just wait for DATA packets
    }

    // else, ignore FEC packets b/c we're in GBN mode

    delete[] fec_buff;
  
  } else if (hdr.ih_type == NETIMG_FIN) { /* NETIMG_FIN */

    int fin_result = recv(sd, (void *) &hdr, sizeof(hdr), 0);
    net_assert(fin_result == -1, "netimg_recvimg() failed to read NETIMG_FIN packet off of wire.\n");

    fprintf(stderr, "netimg_recvimg: received NETIMG_FIN. Sending FIN-ACK\n");
    
    // Create FIN ACK packet
    ihdr_t fin_ack = {
        NETIMG_VERS,
        NETIMG_ACK,
        0,
        htonl(NETIMG_FINSEQ)
    };

    // Probabilistically send FIN-ACK
    if (((float) random())/INT_MAX < pdrop) {
      // Report dropped ACK 
      fprintf(stderr, "\t- DROPPED ACK seqn: %s\n", "NETIMG_FINSEQ");

    } else { /* send segment */
      int fin_ack_result = send(sd, (void *) &fin_ack, sizeof(ihdr_t), 0);
      net_assert(fin_ack_result == -1, "netimg_recvimg() failed to send FIN ACK packet to server\n");
    }
  
  } else { /* error */
    net_assert(0, "netimg_recvimg() invalid type received in packet header\n");
  }
  
  /* give the updated image to OpenGL for texturing */
  glTexImage2D(GL_TEXTURE_2D, 0, (GLint) imsg.im_format,
               (GLsizei) imsg.im_width, (GLsizei) imsg.im_height, 0,
               (GLenum) imsg.im_format, GL_UNSIGNED_BYTE, image);
  /* redisplay */
  glutPostRedisplay();

  return;
}

int
main(int argc, char *argv[])
{
  int err;
  char *sname, *imgname;
  u_short port;

  // parse args, see the comments for netimg_args()
  if (netimg_args(argc, argv, &sname, &port, &imgname)) {
    fprintf(stderr, "Usage: %s -s <server>%c<port> -q <image>.tga [ -w <rwnd [1, 255]> -m <mss (>40)> ]\n", argv[0], NETIMG_PORTSEP); 
    exit(1);
  }

  srandom(NETIMG_SEED);

  socks_init();

  sd = socks_clntinit(sname, port, rwnd*mss);  // Lab5 Task 2

  if (netimg_sendqry(imgname)) {
    err = netimg_recvimsg();

    if (err == NETIMG_FOUND) { // if image received ok
      netimg_glutinit(&argc, argv, netimg_recvimg);
      netimg_imginit(imsg.im_format);
      
      /* Lab5 Task 2: set socket non blocking */
      /* Lab5: YOUR CODE HERE */
      /* DONE */
      net_assert(fcntl(sd, F_SETFL, O_NONBLOCK) == -1, "Failed to set socket to non-blocking");

      glutMainLoop(); /* start the GLUT main loop */
    } else if (err == NETIMG_NFOUND) {
      fprintf(stderr, "%s: %s image not found.\n", argv[0], imgname);
    } else if (err == NETIMG_EVERS) {
      fprintf(stderr, "%s: wrong version number.\n", argv[0]);
    } else if (err == NETIMG_EBUSY) {
      fprintf(stderr, "%s: image server busy.\n", argv[0]);
    } else if (err == NETIMG_ESIZE) {
      fprintf(stderr, "%s: wrong size.\n", argv[0]);
    } else {
      fprintf(stderr, "%s: image receive error %d.\n", argv[0], err);
    }
  }

  socks_close(sd); // optional, but since we use connect(), might as well.
  return(0);
}
