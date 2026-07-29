fetch("/api/v1/system/health")
  .then(async (response) => ({ response, body: await response.json() }))
  .then(({ response, body }) => {
    document.getElementById("health").textContent =
      JSON.stringify(body, null, 2);
    const state = document.getElementById("state");
    const healthy = response.ok && body.data?.status === "ok";
    state.textContent = healthy ? "online" : (body.data?.status ?? "error");
    state.className = healthy ? "ok" : "bad";
  })
  .catch((error) => {
    document.getElementById("health").textContent = String(error);
  });
