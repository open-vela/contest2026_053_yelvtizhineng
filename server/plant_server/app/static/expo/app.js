(function () {
  const form = document.getElementById('feedback-form');
  const status = document.getElementById('form-status');
  const source = new URLSearchParams(location.search).get('src') || '';

  if (!form) return;

  form.addEventListener('submit', async function (event) {
    event.preventDefault();
    status.className = 'form-status';
    status.textContent = '正在提交…';

    const button = form.querySelector('button[type="submit"]');
    button.disabled = true;

    const formData = new FormData(form);
    const payload = {
      name: formData.get('name') || '',
      company: formData.get('company') || '',
      contact: formData.get('contact') || '',
      role: formData.get('role') || '',
      interest: formData.get('interest') || '',
      message: formData.get('message') || '',
      source: source,
      consent: Boolean(formData.get('consent')),
      website: formData.get('website') || ''
    };

    try {
      const response = await fetch('/api/v1/expo/feedback', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
      });
      const data = await response.json();
      if (!response.ok || !data.ok) {
        throw new Error(data.detail || '提交失败，请稍后重试');
      }
      form.reset();
      status.textContent = '已收到，谢谢您的反馈。';
    } catch (error) {
      status.className = 'form-status error';
      status.textContent = error.message || '暂时无法提交，请稍后重试';
    } finally {
      button.disabled = false;
    }
  });
})();
