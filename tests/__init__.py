import logging


LOGGER = logging.getLogger('reaktome')
LOGGER.addHandler(logging.StreamHandler())
LOGGER.setLevel(logging.DEBUG)
