RTMPServer - Video streaming server for Adobe Flash player

Copyright 2012 Janne Kulmala <janne.t.kulmala@iki.fi>

Program code is licensed with GNU LGPL 2.1. See COPYING.LGPL file.

-----

RTMPServer can be used to stream live video/audio content to Adobe Flash 
player clients over RTMP protocol. It is similar to using Adobe's Flash 
Server (FMS). The server is written to be lightweight and easy to 
understand, while having good performance. The server just copies the 
received bitstream to the clients as-is. The server does not support 
authentication of either the clients or the publisher. Written in C++ and 
STL.

Testing with FFmpeg:

    To encode and push a stream:

    ffmpeg -re -i video.mp4 -f flv rtmp://server/live/stream

    To watch the stream:

    ffplay rtmp://server/live/stream
