static const AVBitStreamFilter *bitstream_filters[] = {
    &ff_null_bsf,
    &ff_h264_mp4toannexb_bsf,
    NULL };
