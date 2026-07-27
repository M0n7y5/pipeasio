# Wine PE error-message probe: forces ASIOInit() to fail and verifies that
# GetErrorMessage() reports the concrete failure reason instead of a stub.
pipeasio_add_wine_probe(asio_error)
