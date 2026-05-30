# l4-esp-idf-components

A collection of reusable ESP-IDF 5.x components shared across L4 IoT projects.

## Components

| Component | Description |
|-----------|-------------|
| [`l4_nvs_handler`](components/l4_nvs_handler/README.md) | Generic NVS record handler: validates `{p?, ns, k, t, v}` JSON records, infers partitions, GET/SET/DELETE orchestration, HTTP/MQTT error mapping. |

## Adding a component to your project

### Git dependency (recommended)

In your project's `main/idf_component.yml`:

```yaml
dependencies:
  l4_nvs_handler:
    git: https://github.com/OlegLebedevRU/l4-esp-idf-components.git
    path: components/l4_nvs_handler
```

Then:

```sh
idf.py reconfigure
idf.py build
```

### Local copy

Copy the desired component directory into your project's `components/` folder
and declare it in your `CMakeLists.txt` `REQUIRES` list.

## License

MIT