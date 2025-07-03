variable "tag" {
  default = "ghcr.io/assignuser/velox-dev"
}

target "pyvelox" {
  tags       = ["${tag}:pyvelox"]
  context    = "."
  dockerfile = "scripts/docker/centos-multi.dockerfile"
  target     = "pyvelox"
  args = {
    image = "quay.io/pypa/manylinux_2_28:latest"
  }
  VELOX_BUILD_SHARED = "OFF"
}

# Create targets that match the tags we use to make it easier to create
# the multi-platform images in CI
target "adapters" {
  inherits = ["adapters-cpp"]
}

target "centos9" {
  inherits = ["centos-cpp"]
}

group "ci" {
  targets = ["centos9", "adapters"]
}

group "default" {
  targets = []
}
