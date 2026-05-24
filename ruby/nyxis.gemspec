Gem::Specification.new do |spec|
  spec.name          = "nyxis"
  spec.version       = "1.2.1"
  spec.authors       = ["Micael Malta"]
  spec.email         = ["micael@example.com"]

  spec.summary       = "Zero-copy reader for the Nyxis (NXS) binary format"
  spec.description   = <<~DESC
    Pure-Ruby reader for NXB files produced by the NXS compiler. Provides
    zero-copy memory-mapped access to typed records with O(1) random access
    via the tail-index.
  DESC
  spec.homepage      = "https://github.com/nyxis-io/nyxis-drivers"
  spec.license       = "BUSL-1.1"

  spec.required_ruby_version = ">= 3.0"

  spec.files = [
    "nxs.rb",
    "README.md",
    "LICENSE",
  ]

  spec.require_paths = ["."]

  spec.metadata = {
    "source_code_uri" => "https://github.com/nyxis-io/nyxis-drivers",
    "changelog_uri"   => "https://github.com/nyxis-io/nyxis-drivers/releases",
  }
end
