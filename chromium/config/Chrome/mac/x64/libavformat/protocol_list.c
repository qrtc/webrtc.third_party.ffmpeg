static const URLProtocol *url_protocols[] = {
    &ff_file_protocol,
    &ff_http_protocol,
    &ff_https_protocol,
    &ff_tls_openssl_protocol,
    &ff_pipe_protocol,
    &ff_tcp_protocol,
    NULL 
};