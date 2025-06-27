variable "tag" {
  default = "ghcr.io/facebookincubator/velox-dev"
}

function "cache-to-arch" {
  params = [name, arch]
  result = {
    type        = "registry"
    ref         = "${tag}:build-cache-${name}-${arch}"
    mode        = "max"
    compression = "zstd"
  }
}

function "cache-from-arch" {
  params = [name, arch]
  result = {
    type = "registry"
    ref  = "${tag}:build-cache-${name}-${arch}"
  }
}

target "pyvelox" {
  name       = "pyvelox-${arch}"
  context    = "."
  dockerfile = "scripts/docker/centos-multi.dockerfile"
  target     = "pyvelox"
  args = {
    image              = "quay.io/pypa/manylinux_2_28:latest"
    VELOX_BUILD_SHARED = "OFF"
  }
  matrix = {
    arch = ["amd64", "arm64"]
  }
  platforms  = ["linux/${arch}"]
  tags       = ["${tag}:pyvelox-${arch}"]
  cache-to   = [cache-to-arch("pyvelox", "${arch}")]
  cache-from = [cache-from-arch("pyvelox", "${arch}")]
}

target "adapters" {
  inherits = ["adapters-cpp"]
  name     = "adapters-${arch}"
  matrix = {
    arch = ["amd64", "arm64"]
  }
  platforms  = ["linux/${arch}"]
  tags       = ["${tag}:adapters-${arch}"]
  cache-to   = [cache-to-arch("adapters", "${arch}")]
  cache-from = [cache-from-arch("adapters", "${arch}")]
}

target "centos9" {
  inherits = ["centos-cpp"]
  name     = "centos9-${arch}"
  matrix = {
    arch = ["amd64", "arm64"]
  }
  platforms  = ["linux/${arch}"]
  tags       = ["${tag}:centos9-${arch}"]
  cache-to   = [cache-to-arch("centos9", "${arch}")]
  cache-from = [cache-from-arch("centos9", "${arch}")]
}

function "ci_images_by_arch" {
  params = [arch]
  result = ["centos9-${arch}", "adapters-${arch}"]
}

group "ci-amd64" {
  targets = ci_images_by_arch("amd64")
}

group "ci-arm64" {
  targets = ci_images_by_arch("arm64")
}

group "default" {
  targets = []
}
