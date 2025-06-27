variable "tag" {
  default = "ghcr.io/facebookincubator/velox-dev"
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

group "multi-platform" {
  targets = ["centos-cpp", "adapters-cpp", "pyvelox"]
}

group "default" {
  targets = []
}
