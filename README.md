# prismriver-driver
Driver para Linux que dá suporte ao controle guitarra do Guitar Hero 5

## Projeto final da matéria de Interface-Hardware-Software
Aluno: Davi Araújo do Nascimento

Professor: Bruno Otávio Piedade Prado

## Etapas de desenvolvimento
- [x] Estudo e testes de drivers de HID para Linux
- [ ] Driver reconhece comandos dados no controle
- [ ] Driver traduz os comandos para comandos de teclado
- [ ] ~Driver traduz os comandos para comandos dentro do Clone Hero~
- [ ] ~Aplicativo de terminal feito~
- [ ] Finalização e organização

### Mudança nos planos
O foco agora será alterar o driver existente do linux hid-sony.c para digitar como um teclado.

## Ferramentas/Recursos
https://gitlab.freedesktop.org/libevdev/hid-tools
https://docs.kernel.org/hid/hidintro.html
https://bruno.dcomp.ufs.br/aulas/ihs/ihs_12_gerenciadores_dispositivos.pdf
https://bruno.dcomp.ufs.br/aulas/ihs/ihs_11_gerenciadores_dispositivos.pdf
https://github.com/rizsotto/Bear
https://github.com/torvalds/linux/blob/master/drivers/hid/hid-sony.c
