package com.example.belkarx;

public interface AudioCable {
    void send(float[] sample); // one multi-channel sample
    void endOfFrame();
    void endOfStream();
}
