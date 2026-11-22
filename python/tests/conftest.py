import os

# The exit hook of raccoon_transport ends the process with os._exit(); a test run keeps the
# exit status of the runner instead.
os.environ["RACCOON_TRANSPORT_NO_EXIT_HOOK"] = "1"
