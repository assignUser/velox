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

group "ci" {
  targets = ["centos-cpp", "adapters-cpp"]
}

group "default" {
  targets = []
}
