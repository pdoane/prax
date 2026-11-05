# A job for a scheduler, in HCL's own blocks and expressions.
job "web" {
  datacenters = ["east-1", "east-2"]
  type        = "service"
  priority    = 50

  group "frontend" {
    count = 3

    network {
      port "http" {
        to = 8080
      }
    }

    task "server" {
      driver = "docker"

      config {
        image = "registry.example.com/web:${var.version}"
        ports = ["http"]
        args  = [for flag, value in var.flags : "--${flag}=${value}"]
      }

      env {
        LOG_LEVEL = var.debug ? "debug" : "info"
        REPLICAS  = count * 2
      }

      resources {
        cpu    = 500 # MHz
        memory = 256
      }
    }
  }
}
